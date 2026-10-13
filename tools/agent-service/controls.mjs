export const THINKING_LEVELS = ['off', 'minimal', 'low', 'medium', 'high', 'xhigh', 'max'];

export function validateControls(controls, contextWindow) {
    if (!controls || !THINKING_LEVELS.includes(controls.thinkingLevel)) {
        throw new Error('thinkingLevel must be off, minimal, low, medium, high, xhigh, or max');
    }
    if (!Number.isSafeInteger(controls.outputTokens) || controls.outputTokens <= 0 || controls.outputTokens >= contextWindow) {
        throw new Error('outputTokens must be a positive integer smaller than the loaded context');
    }
    const compaction = controls.compaction;
    if (!compaction || typeof compaction.enabled !== 'boolean') {
        throw new Error('compaction.enabled must be a boolean');
    }
    if (!Number.isSafeInteger(compaction.reserveTokens) || compaction.reserveTokens <= 0) {
        throw new Error('compaction.reserveTokens must be a positive integer');
    }
    if (!Number.isSafeInteger(compaction.keepRecentTokens) || compaction.keepRecentTokens < 0) {
        throw new Error('compaction.keepRecentTokens must be a nonnegative integer');
    }
    if (compaction.reserveTokens + compaction.keepRecentTokens >= contextWindow) {
        throw new Error('Compaction reserve and recent history must leave space in the loaded context');
    }
    if (compaction.reserveTokens < controls.outputTokens) {
        throw new Error('Compaction reserve must cover outputTokens');
    }
    return {
        thinkingLevel: controls.thinkingLevel,
        outputTokens: controls.outputTokens,
        compaction: {
            enabled: compaction.enabled,
            reserveTokens: compaction.reserveTokens,
            keepRecentTokens: compaction.keepRecentTokens,
        },
    };
}
