import { createInterface } from 'node:readline';
import { AgentService } from './service.mjs';

const service = new AgentService();
const pending = new Set();
let opening = false;
let closing = false;
let runId = null;

function send(frame) {
    process.stdout.write(`${JSON.stringify(frame)}\n`);
}

service.subscribe(event => send({ type: 'event', runId, event }));

async function dispatch(request) {
    const { id, method, params } = request;
    if (typeof id !== 'string' || id.length === 0) {
        throw new Error('Each request needs a nonempty string id');
    }
    if (opening) {
        throw new Error('Session is opening; wait for its response');
    }
    switch (method) {
        case 'open':
            opening = true;
            try {
                return await service.open(params);
            }
            finally {
                opening = false;
            }
        case 'get_state':
            return service.getState();
        case 'configure':
            return service.configure(params);
        case 'prompt':
        case 'compact': {
            if (service.getState().busy) {
                throw new Error('Session is busy; wait or abort before starting another run');
            }
            runId = id;
            try {
                if (method === 'prompt') {
                    return await service.prompt(params?.message);
                }
                return await service.compact(params?.instructions);
            }
            finally {
                runId = null;
            }
        }
        case 'abort':
            await service.abort();
            return {};
        case 'close':
            closing = true;
            try {
                await service.close();
                return {};
            }
            finally {
                closing = false;
            }
        default:
            throw new Error(`Unknown method: ${method}`);
    }
}

const input = createInterface({ input: process.stdin, crlfDelay: Infinity, terminal: false });
for await (const line of input) {
    let request;
    try {
        request = JSON.parse(line);
        if (!request || typeof request !== 'object' || Array.isArray(request)) {
            throw new Error('Request must be an object');
        }
    }
    catch (error) {
        send({ type: 'response', id: null, ok: false, error: error.message });
        continue;
    }
    const operation = dispatch(request).then(
        data => send({ type: 'response', id: request.id ?? null, ok: true, data }),
        error => send({ type: 'response', id: request.id ?? null, ok: false, error: error.message }),
    );
    pending.add(operation);
    operation.finally(() => pending.delete(operation));
}

if (opening || closing) {
    await Promise.allSettled(pending);
}
await service.close();
await Promise.allSettled(pending);
await service.close();
