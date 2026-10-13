import assert from 'node:assert/strict';
import { mkdtemp, rm } from 'node:fs/promises';
import { createServer } from 'node:http';
import { spawn } from 'node:child_process';
import { once } from 'node:events';
import { createInterface } from 'node:readline';
import { tmpdir } from 'node:os';
import { join, resolve, sep } from 'node:path';
import { test } from 'node:test';
import { AgentService } from '../service.mjs';

async function fixture(t, { thinking = 'toggle' } = {}) {
    const cwd = await mkdtemp(join(tmpdir(), 'fbdb-agent-test-'));
    const calls = [];
    const state = {
        modelId: 'fixture/qwen:Q4',
        status: 'loaded',
        contextWindow: 8192,
        modelPath: '/synthetic/model.gguf',
        rejectInference: false,
        toolNext: false,
        holdInference: false,
        inferenceStarted: undefined,
        finishReason: 'stop',
        toolCalling: true,
    };
    const server = createServer(async (request, response) => {
        const url = new URL(request.url, 'http://localhost');
        calls.push({ method: request.method, url });
        response.setHeader('Content-Type', 'application/json');
        if (url.pathname === '/models') {
            response.end(JSON.stringify({ data: [{ id: state.modelId, status: { value: state.status } }] }));
            return;
        }
        if (url.pathname === '/props') {
            assert.equal(url.searchParams.get('model'), state.modelId);
            assert.equal(url.searchParams.get('autoload'), 'false');
            response.end(JSON.stringify({
                model_path: state.modelPath,
                default_generation_settings: { n_ctx: state.contextWindow },
                chat_template: thinking === 'toggle' ? '{{ enable_thinking }}' : 'fixture template',
                chat_template_caps: { supports_tools: state.toolCalling, supports_tool_calls: state.toolCalling, supports_reasoning_effort: thinking === 'effort' },
                build_info: 'synthetic-server',
            }));
            return;
        }
        if (url.pathname !== '/v1/chat/completions') {
            response.writeHead(404).end();
            return;
        }
        let body = '';
        for await (const chunk of request) {
            body += chunk;
        }
        const payload = JSON.parse(body);
        calls.at(-1).payload = payload;
        if (state.holdInference) {
            response.setHeader('Content-Type', 'text/event-stream');
            response.flushHeaders();
            state.inferenceStarted();
            return;
        }
        if (state.rejectInference) {
            response.writeHead(400).end(JSON.stringify({ error: { message: 'fixture inference failure' } }));
            return;
        }
        response.setHeader('Content-Type', 'text/event-stream');
        const toolCall = state.toolNext;
        state.toolNext = false;
        const delta = toolCall
            ? { tool_calls: [{ index: 0, id: 'fixture-call', type: 'function', function: { name: 'read', arguments: JSON.stringify({ path: 'missing-fixture.txt' }) } }] }
            : { content: 'Fixture reply' };
        response.write(`data: ${JSON.stringify({ id: 'fixture', object: 'chat.completion.chunk', model: state.modelId, choices: [{ index: 0, delta, finish_reason: null }] })}\n\n`);
        response.write(`data: ${JSON.stringify({ id: 'fixture', object: 'chat.completion.chunk', model: state.modelId, choices: [{ index: 0, delta: {}, finish_reason: toolCall ? 'tool_calls' : state.finishReason }], usage: { prompt_tokens: 120, completion_tokens: 4, total_tokens: 124 } })}\n\n`);
        response.end('data: [DONE]\n\n');
    });
    await new Promise(resolve => server.listen(0, '127.0.0.1', resolve));
    const service = new AgentService();
    t.after(async () => {
        await service.close();
        server.closeAllConnections();
        await new Promise(resolve => server.close(resolve));
        assert.ok(resolve(cwd).startsWith(`${resolve(tmpdir())}${sep}fbdb-agent-test-`));
        await rm(cwd, { recursive: true, force: true });
    });
    const options = {
        cwd,
        routerUrl: `http://127.0.0.1:${server.address().port}`,
        modelId: state.modelId,
        tools: [],
        effortLevels: thinking === 'effort' ? ['low', 'medium', 'high'] : [],
        controls: { thinkingLevel: 'high', compaction: { enabled: true, reserveTokens: 2048, keepRecentTokens: 2048 }, outputTokens: 1024 },
    };
    return { service, state, calls, options };
}

test('binds exact router model and reports effective toggle thinking', async t => {
    const { service, calls, options } = await fixture(t);
    const snapshot = await service.open(options);
    assert.equal(snapshot.binding.modelId, options.modelId);
    assert.equal(snapshot.binding.contextWindow, 8192);
    assert.equal(snapshot.controls.requestedThinkingLevel, 'high');
    assert.equal(snapshot.controls.effectiveThinkingLevel, 'medium');
    assert.deepEqual(snapshot.controls.availableThinkingLevels, ['off', 'medium']);
    const result = await service.prompt('Synthetic test only');
    assert.equal(result.status, 'completed');
    const inference = calls.filter(call => call.payload);
    assert.equal(inference.length, 1);
    assert.equal(inference[0].payload.model, options.modelId);
    assert.equal(inference[0].payload.max_tokens, 1024);
    assert.equal(inference[0].payload.chat_template_kwargs.enable_thinking, true);
    assert.ok(calls.every(call => call.method === 'GET' || call.url.pathname === '/v1/chat/completions'));
});

test('thinking off and compaction changes reach Pi without changing the model', async t => {
    const { service, calls, options } = await fixture(t);
    await service.open(options);
    const snapshot = service.configure({ thinkingLevel: 'off', compaction: { enabled: false, reserveTokens: 1536, keepRecentTokens: 1024 } });
    assert.equal(snapshot.controls.effectiveThinkingLevel, 'off');
    assert.deepEqual(snapshot.controls.compaction, { enabled: false, reserveTokens: 1536, keepRecentTokens: 1024 });
    await service.prompt('Synthetic off test');
    assert.equal(calls.find(call => call.payload).payload.chat_template_kwargs.enable_thinking, false);
});

test('effort-capable template receives the requested effort', async t => {
    const { service, calls, options } = await fixture(t, { thinking: 'effort' });
    const snapshot = await service.open(options);
    assert.equal(snapshot.controls.effectiveThinkingLevel, 'high');
    await service.prompt('Synthetic effort test');
    assert.equal(calls.find(call => call.payload).payload.reasoning_effort, 'high');
});

test('nonreasoning model reports off', async t => {
    const { service, options } = await fixture(t, { thinking: 'none' });
    const snapshot = await service.open(options);
    assert.deepEqual(snapshot.controls.availableThinkingLevels, ['off']);
    assert.equal(snapshot.controls.effectiveThinkingLevel, 'off');
});

test('Pi owns the tool call and continues after a tool error', async t => {
    const { service, state, calls, options } = await fixture(t);
    options.tools = ['read'];
    state.toolNext = true;
    const events = [];
    service.subscribe(event => events.push(event));
    await service.open(options);
    const result = await service.prompt('Read missing-fixture.txt');
    assert.equal(result.status, 'completed');
    assert.equal(calls.filter(call => call.payload).length, 2);
    assert.ok(events.some(event => event.type === 'tool_execution_end' && event.isError));
    assert.ok(events.some(event => event.type === 'agent_settled'));
});

test('missing and unloaded models fail without loading or selecting another', async t => {
    const { service, state, calls, options } = await fixture(t);
    await assert.rejects(service.open({ ...options, modelId: 'not-the-selected-model' }), /not found/);
    state.status = 'unloaded';
    await assert.rejects(service.open(options), /not loaded/);
    assert.ok(calls.every(call => call.method === 'GET'));
});

test('rejects invalid controls before inference and keeps previous settings', async t => {
    const { service, calls, options } = await fixture(t);
    await service.open(options);
    for (const reserveTokens of [-1, '2048', 8192, 1.5]) {
        assert.throws(() => service.configure({ thinkingLevel: 'off', compaction: { enabled: true, reserveTokens, keepRecentTokens: 2048 } }));
        assert.equal(service.getState().controls.effectiveThinkingLevel, 'medium');
    }
    assert.throws(() => service.configure({ thinkingLevel: 'invented-level' }), /thinkingLevel/);
    assert.ok(calls.every(call => !call.payload));
});

test('changed backend binding requires reopening the session', async t => {
    const { service, state, calls, options } = await fixture(t);
    await service.open(options);
    state.contextWindow = 4096;
    await assert.rejects(service.prompt('Must not run'), /binding changed/);
    assert.ok(calls.every(call => !call.payload));
});

test('inference error is a failed run, not verified success', async t => {
    const { service, state, options } = await fixture(t);
    await service.open(options);
    state.rejectInference = true;
    const result = await service.prompt('Synthetic failing run');
    assert.equal(result.status, 'failed');
    assert.match(result.error, /fixture inference failure/);
});

test('manual compaction uses the bound model and explicit token settings', async t => {
    const { service, state, calls, options } = await fixture(t);
    state.contextWindow = 32768;
    await service.open(options);
    for (let turn = 0; turn < 5; turn++) {
        await service.prompt('Synthetic history '.repeat(450));
    }
    const result = await service.compact('Keep the synthetic task and observed errors');
    assert.equal(result.summary, 'Fixture reply');
    const lastInference = calls.filter(call => call.payload).at(-1).payload;
    assert.equal(lastInference.model, options.modelId);
    assert.equal(lastInference.max_tokens, 1024);
    assert.equal(service.getState().lastRequest.outputTokens, 1024);
});

test('reports Pi reducing the output cap when summary context leaves little room', async t => {
    const { service, calls, options } = await fixture(t);
    await service.open(options);
    for (let turn = 0; turn < 5; turn++) {
        await service.prompt('Synthetic history '.repeat(450));
    }
    await service.compact();
    const cap = calls.filter(call => call.payload).at(-1).payload.max_tokens;
    assert.equal(cap, 1);
    assert.equal(service.getState().lastRequest.outputTokens, cap);
});

test('abort interrupts inference while concurrent prompt and setting changes are rejected', { timeout: 10000 }, async t => {
    const { service, state, options } = await fixture(t);
    await service.open(options);
    state.holdInference = true;
    const started = new Promise(resolve => { state.inferenceStarted = resolve; });
    const running = service.prompt('Synthetic pending request');
    await started;
    await assert.rejects(service.prompt('Concurrent request'), /busy/);
    assert.throws(() => service.configure({ thinkingLevel: 'off' }), /busy/);
    await service.abort();
    assert.equal((await running).status, 'aborted');
    assert.equal(service.getState().busy, false);
});

test('persistent session can resume in its original workspace', async t => {
    const { service, options } = await fixture(t);
    const snapshot = await service.open({ ...options, sessionDirectory: join(options.cwd, 'sessions') });
    await service.prompt('Synthetic continuity');
    const sessionFile = snapshot.sessionFile;
    await service.close();
    const resumed = await service.open({ ...options, sessionFile });
    assert.equal(resumed.sessionId, snapshot.sessionId);
    assert.equal(resumed.binding.modelId, snapshot.binding.modelId);
});

test('open rejects malformed workspace, tools and incomplete controls', async t => {
    const { service, options } = await fixture(t);
    await assert.rejects(service.open(), /cwd/);
    await assert.rejects(service.open({ ...options, cwd: '.' }), /absolute/);
    await assert.rejects(service.open({ ...options, tools: ['made-up-tool'] }), /tools/);
    await assert.rejects(service.open({ ...options, controls: {} }), /thinkingLevel/);
});

test('token-limit termination is not reported as a completed reply', async t => {
    const { service, state, options } = await fixture(t);
    options.controls.compaction.enabled = false;
    state.finishReason = 'length';
    await service.open(options);
    const result = await service.prompt('Synthetic truncated request');
    assert.equal(result.status, 'incomplete');
    assert.equal(result.stopReason, 'length');
});

test('tool-enabled session requires the template to support tool declarations and calls', async t => {
    const { service, state, options } = await fixture(t);
    state.toolCalling = false;
    await assert.rejects(service.open({ ...options, tools: ['read'] }), /tool support/);
});

test('lifecycle transitions reject competing open and prompt operations', async t => {
    const { service, options } = await fixture(t);
    const opening = service.open(options);
    await assert.rejects(service.open(options), /Close the current session/);
    await opening;
    const closing = service.close();
    await assert.rejects(service.prompt('Must not race close'), /busy/);
    await closing;
});

test('worker streams events, answers during a run, and accepts abort through JSON lines', { timeout: 10000 }, async t => {
    const { state, options } = await fixture(t);
    const child = spawn(process.execPath, ['worker.mjs'], { cwd: new URL('..', import.meta.url), stdio: ['pipe', 'pipe', 'pipe'] });
    const exited = once(child, 'exit');
    const frames = [];
    const responses = new Map();
    let diagnostics = '';
    child.stderr.on('data', chunk => { diagnostics += chunk; });
    const output = createInterface({ input: child.stdout });
    output.on('line', line => {
        const frame = JSON.parse(line);
        frames.push(frame);
        if (frame.type === 'response') {
            responses.get(frame.id)?.(frame);
            responses.delete(frame.id);
        }
    });
    function send(id, method, params) {
        const response = new Promise(resolve => responses.set(id, resolve));
        child.stdin.write(`${JSON.stringify({ id, method, params })}\r\n`);
        return response;
    }
    t.after(async () => {
        child.stdin.end();
        const [code] = await exited;
        assert.equal(code, 0, diagnostics);
    });
    const invalid = new Promise(resolve => responses.set(null, resolve));
    child.stdin.write('not-json\n');
    assert.equal((await invalid).ok, false);
    assert.equal((await send('unknown', 'made-up-method')).ok, false);
    assert.equal((await send('open', 'open', options)).ok, true);
    state.holdInference = true;
    const started = new Promise(resolve => { state.inferenceStarted = resolve; });
    const run = send('run', 'prompt', { message: 'Synthetic worker request' });
    await started;
    const snapshot = await send('state', 'get_state');
    assert.equal(snapshot.data.busy, true);
    assert.equal((await send('settings', 'configure', { thinkingLevel: 'off' })).ok, false);
    assert.equal((await send('cancel', 'abort')).ok, true);
    assert.equal((await run).data.status, 'aborted');
    assert.ok(frames.some(frame => frame.type === 'event' && frame.runId === 'run' && frame.event.type === 'request_settings'));
    assert.equal((await send('close', 'close')).ok, true);
});
