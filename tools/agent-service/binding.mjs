export async function inspectBinding(routerUrl, modelId, signal) {
    if (typeof modelId !== 'string' || modelId.length === 0) {
        throw new Error('modelId must be the exact router preset ID');
    }
    const base = new URL(routerUrl);
    if (!['http:', 'https:'].includes(base.protocol) || base.username || base.password) {
        throw new Error('routerUrl must be an HTTP URL without embedded credentials');
    }
    if (base.pathname !== '/' || base.search || base.hash) {
        throw new Error('routerUrl must point to the router root');
    }

    async function getJson(path) {
        const timeout = AbortSignal.timeout(15000);
        const response = await fetch(new URL(path, base), { signal: signal ? AbortSignal.any([signal, timeout]) : timeout });
        if (!response.ok) {
            throw new Error(`Router ${path.split('?')[0]} returned HTTP ${response.status}`);
        }
        return response.json();
    }

    const catalog = await getJson('/models');
    const model = catalog.data?.find(entry => entry.id === modelId);
    if (!model) {
        throw new Error(`Model ${modelId} not found in the router`);
    }
    if (model.status?.value !== 'loaded') {
        throw new Error(`Model ${modelId} is not loaded; load it through the backend first`);
    }
    const props = await getJson(`/props?${new URLSearchParams({ model: modelId, autoload: 'false' })}`);
    const contextWindow = props.default_generation_settings?.n_ctx;
    if (!Number.isSafeInteger(contextWindow) || contextWindow <= 0) {
        throw new Error('Router did not report a positive loaded context size');
    }
    if (typeof props.model_path !== 'string' || props.model_path.length === 0) {
        throw new Error('Router did not report the loaded model path');
    }
    return {
        routerUrl: base.origin,
        modelId,
        modelPath: props.model_path,
        contextWindow,
        serverBuild: props.build_info ?? null,
        toolCalling: props.chat_template_caps?.supports_tools === true
            && props.chat_template_caps?.supports_tool_calls === true,
        thinking: props.chat_template_caps?.supports_reasoning_effort === true
            ? 'effort'
            : props.chat_template?.includes('enable_thinking') === true ? 'toggle' : 'none',
        vision: props.modalities?.vision === true,
    };
}

export function sameBinding(left, right) {
    return left.routerUrl === right.routerUrl
        && left.modelId === right.modelId
        && left.modelPath === right.modelPath
        && left.contextWindow === right.contextWindow
        && left.serverBuild === right.serverBuild
        && left.thinking === right.thinking
        && left.toolCalling === right.toolCalling;
}
