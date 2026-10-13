import { stat } from 'node:fs/promises';
import { isAbsolute, join, resolve } from 'node:path';
import { InMemoryCredentialStore } from '@earendil-works/pi-ai';
import { streamSimple } from '@earendil-works/pi-ai/compat';
import { createAgentSession, DefaultResourceLoader, ModelRuntime, SessionManager, SettingsManager } from '@earendil-works/pi-coding-agent';
import { inspectBinding, sameBinding } from './binding.mjs';
import { THINKING_LEVELS, validateControls } from './controls.mjs';

const PROVIDER = 'fbdb-local';
const BUILTIN_TOOLS = ['read', 'edit', 'write', 'bash', 'powershell', 'grep', 'find', 'ls'];
const STOP_STATUS = { error: 'failed', aborted: 'aborted', length: 'incomplete' };

function providerModel(binding, controls, effortLevels) {
    const reasoning = binding.thinking !== 'none';
    const thinkingLevelMap = Object.fromEntries(THINKING_LEVELS.map(level => [level, null]));
    if (binding.thinking === 'toggle') {
        thinkingLevelMap.off = 'off';
        thinkingLevelMap.medium = 'medium';
    }
    if (binding.thinking === 'effort') {
        thinkingLevelMap.off = 'none';
        for (const level of effortLevels) {
            thinkingLevelMap[level] = level;
        }
    }
    return {
        id: binding.modelId,
        name: binding.modelId,
        reasoning,
        thinkingLevelMap,
        input: binding.vision ? ['text', 'image'] : ['text'],
        cost: { input: 0, output: 0, cacheRead: 0, cacheWrite: 0 },
        contextWindow: binding.contextWindow,
        maxTokens: controls.outputTokens,
        compat: {
            supportsStore: false,
            supportsDeveloperRole: false,
            supportsReasoningEffort: binding.thinking === 'effort',
            supportsStrictMode: false,
            supportsUsageInStreaming: true,
            maxTokensField: 'max_tokens',
            ...(binding.thinking === 'toggle' ? { thinkingFormat: 'qwen-chat-template' } : {}),
        },
    };
}

export class AgentService {
    #session;
    #settings;
    #binding;
    #controls;
    #active;
    #abortController;
    #lastRequest;
    #transition = false;
    #listeners = new Set();

    subscribe(listener) {
        this.#listeners.add(listener);
        return () => this.#listeners.delete(listener);
    }

    async open(options) {
        if (this.#session || this.#active || this.#transition) {
            throw new Error('Close the current session before opening another');
        }
        this.#transition = true;
        try {
            return await this.#open(options);
        }
        finally {
            this.#transition = false;
        }
    }

    async #open(options) {
        if (!options || typeof options.cwd !== 'string' || !isAbsolute(options.cwd)) {
            throw new Error('cwd must be an absolute workspace directory');
        }
        if (!(await stat(options.cwd)).isDirectory()) {
            throw new Error('cwd must be a directory');
        }
        if (!Array.isArray(options.tools) || options.tools.some(name => !BUILTIN_TOOLS.includes(name))) {
            throw new Error('tools must explicitly list supported Pi tool names');
        }
        const binding = await inspectBinding(options.routerUrl, options.modelId);
        const controls = validateControls(options.controls, binding.contextWindow);
        const effortLevels = options.effortLevels ?? [];
        if (!Array.isArray(effortLevels) || effortLevels.some(level => !THINKING_LEVELS.includes(level) || level === 'off')) {
            throw new Error('effortLevels must list the confirmed non-off thinking levels for this model');
        }
        if (options.tools.length > 0 && !binding.toolCalling) {
            throw new Error('The loaded template does not report tool support');
        }
        const cwd = resolve(options.cwd);
        let sessionManager;
        if (options.sessionFile) {
            if (!isAbsolute(options.sessionFile)) {
                throw new Error('sessionFile must be absolute');
            }
            sessionManager = SessionManager.open(options.sessionFile);
            if (resolve(sessionManager.getCwd()) !== cwd) {
                throw new Error('Session belongs to another workspace');
            }
        }
        else if (options.sessionDirectory) {
            if (!isAbsolute(options.sessionDirectory)) {
                throw new Error('sessionDirectory must be absolute');
            }
            sessionManager = SessionManager.create(cwd, options.sessionDirectory);
        }
        else {
            sessionManager = SessionManager.inMemory(cwd);
        }
        const settingsManager = SettingsManager.inMemory({ compaction: controls.compaction, cacheWarming: 'off' });
        const modelRuntime = await ModelRuntime.create({
            credentials: new InMemoryCredentialStore(),
            modelsPath: null,
            refreshOnCreate: false,
            allowModelNetwork: false,
        });
        modelRuntime.registerProvider(PROVIDER, {
            api: 'openai-completions',
            baseUrl: `${binding.routerUrl}/v1`,
            apiKey: 'local',
            authHeader: false,
            models: [providerModel(binding, controls, effortLevels)],
            streamSimple: (model, context, options) => streamSimple(model, context, {
                ...options,
                onPayload: async (payload, requestModel) => {
                    const transformed = await options?.onPayload?.(payload, requestModel);
                    const effective = transformed ?? payload;
                    this.#lastRequest = {
                        modelId: effective.model,
                        outputTokens: effective.max_tokens,
                        reasoningEffort: effective.reasoning_effort ?? null,
                        thinkingEnabled: effective.chat_template_kwargs?.enable_thinking ?? null,
                    };
                    for (const listener of this.#listeners) {
                        listener({ type: 'request_settings', ...this.#lastRequest });
                    }
                    return transformed;
                },
            }),
        });
        const model = modelRuntime.getModel(PROVIDER, binding.modelId);
        if (!model) {
            throw new Error('Pi did not register the exact backend model');
        }
        const agentDir = join(cwd, '.fbdb', 'agent');
        const resourceLoader = new DefaultResourceLoader({
            cwd,
            agentDir,
            settingsManager,
            noExtensions: true,
            noSkills: true,
            noPromptTemplates: true,
            noThemes: true,
        });
        await resourceLoader.reload();
        const { session } = await createAgentSession({
            cwd,
            agentDir,
            modelRuntime,
            model,
            thinkingLevel: controls.thinkingLevel,
            tools: options.tools,
            settingsManager,
            sessionManager,
            resourceLoader,
        });
        this.#session = session;
        this.#settings = settingsManager;
        this.#binding = binding;
        this.#controls = controls;
        session.subscribe(event => {
            for (const listener of this.#listeners) {
                listener(event);
            }
        });
        return this.getState();
    }

    getState() {
        this.#requireSession();
        return {
            binding: { ...this.#binding },
            sessionId: this.#session.sessionManager.getSessionId(),
            sessionFile: this.#session.sessionManager.getSessionFile() ?? null,
            busy: Boolean(this.#active),
            tools: this.#session.getActiveToolNames(),
            controls: {
                requestedThinkingLevel: this.#controls.thinkingLevel,
                effectiveThinkingLevel: this.#session.thinkingLevel,
                availableThinkingLevels: this.#session.getAvailableThinkingLevels(),
                outputTokens: this.#controls.outputTokens,
                compaction: this.#settings.getCompactionSettings(this.#session.model),
            },
            contextUsage: this.#session.getContextUsage() ?? null,
            lastRequest: this.#lastRequest ? { ...this.#lastRequest } : null,
        };
    }

    configure(changes) {
        this.#requireIdle();
        if (!changes || Object.keys(changes).some(key => !['thinkingLevel', 'compaction'].includes(key))) {
            throw new Error('configure accepts thinkingLevel and compaction');
        }
        const controls = validateControls({ ...this.#controls, ...changes }, this.#binding.contextWindow);
        this.#settings.applyOverrides({ compaction: controls.compaction });
        this.#session.setThinkingLevel(controls.thinkingLevel);
        this.#controls = controls;
        return this.getState();
    }

    async #checkBinding(signal) {
        const current = await inspectBinding(this.#binding.routerUrl, this.#binding.modelId, signal);
        if (!sameBinding(this.#binding, current)) {
            throw new Error('Backend binding changed; reopen the session with its new settings');
        }
    }

    async #run(operation) {
        this.#requireIdle();
        this.#abortController = new AbortController();
        const signal = this.#abortController.signal;
        const active = (async () => {
            await this.#checkBinding(signal);
            signal.throwIfAborted();
            return operation();
        })();
        this.#active = active;
        try {
            return await active;
        }
        finally {
            this.#active = undefined;
            this.#abortController = undefined;
        }
    }

    async prompt(message) {
        if (typeof message !== 'string' || message.trim().length === 0) {
            throw new Error('message must be nonempty text');
        }
        return this.#run(async () => {
            await this.#session.prompt(message);
            await this.#session.waitForIdle();
            const last = this.#session.messages.findLast(entry => entry.role === 'assistant');
            return {
                status: STOP_STATUS[last?.stopReason] ?? 'completed',
                stopReason: last?.stopReason ?? null,
                error: last?.errorMessage ?? null,
                text: this.#session.getLastAssistantText() ?? '',
            };
        });
    }

    async compact(instructions) {
        if (instructions !== undefined && typeof instructions !== 'string') {
            throw new Error('instructions must be text');
        }
        return this.#run(() => this.#session.compact(instructions));
    }

    async abort() {
        this.#requireSession();
        this.#abortController?.abort();
        await this.#session.abort();
        if (this.#active) {
            await this.#active.catch(() => {});
        }
    }

    async close() {
        if (this.#transition) {
            throw new Error('Session lifecycle is busy; wait for its response');
        }
        if (!this.#session) {
            return;
        }
        this.#transition = true;
        try {
            await this.abort();
            this.#session.dispose();
            this.#session = undefined;
            this.#settings = undefined;
            this.#binding = undefined;
            this.#controls = undefined;
            this.#lastRequest = undefined;
        }
        finally {
            this.#transition = false;
        }
    }

    #requireSession() {
        if (!this.#session) {
            throw new Error('Open a session first');
        }
    }

    #requireIdle() {
        this.#requireSession();
        if (this.#active || this.#transition) {
            throw new Error('Session is busy; wait or abort before changing it');
        }
    }
}
