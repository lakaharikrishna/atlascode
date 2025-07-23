import { Container } from '../container';

interface SpliceContext {
    file: string;
    function: string;
    arrayType: string;
    arrayLength: number;
    startIndex: number;
    deleteCount: number;
    timestamp: string;
}

const buildContext = (
    array: any,
    start: number,
    deleteCount: number,
    context: Partial<SpliceContext>,
): SpliceContext => ({
    file: context.file ?? 'unknown',
    function: context.function ?? 'unknown',
    arrayType: typeof array,
    arrayLength: Array.isArray(array) ? array.length : 0,
    startIndex: start,
    deleteCount,
    timestamp: new Date().toISOString(),
});

const trackEvent = (action: string, context: SpliceContext, extra: Record<string, any>): void => {
    Container.analyticsClient.sendTrackEvent({
        trackEvent: {
            action,
            actionSubject: 'array_operation',
            attributes: {
                context,
                ...extra,
            },
        },
    });
};

const logAndReturnFallback = <T>(message: string, context: SpliceContext): T[] => {
    trackEvent('splice_error', context, {
        error: {
            message,
            stack: new Error().stack,
        },
    });

    return [];
};

const attemptFallbackSplice = <T>(
    array: T[],
    start: number,
    deleteCount: number,
    items: T[],
    context: SpliceContext,
    originalError: Error,
): T[] => {
    trackEvent('splice_error', context, {
        error: {
            message: originalError.message,
            stack: originalError.stack,
        },
    });

    try {
        const fallbackResult = Array.prototype.splice.call(array, start, deleteCount, ...items);

        trackEvent('splice_fallback_success', context, {
            result: {
                removedItems: fallbackResult.length,
                newArrayLength: array.length,
            },
        });

        return fallbackResult;
    } catch (fallbackError: any) {
        trackEvent('splice_fallback_error', context, {
            error: {
                message: fallbackError.message,
                stack: fallbackError.stack,
            },
        });

        return [];
    }
};

export const safeSplice = <T>(
    array: T[],
    start: number,
    deleteCount: number,
    context: Partial<SpliceContext> = {},
    ...items: T[]
): T[] => {
    const fullContext = buildContext(array, start, deleteCount, context);

    if (!Array.isArray(array)) {
        return logAndReturnFallback(`Not an array: ${typeof array}`, fullContext);
    }

    if (typeof array.splice !== 'function') {
        return logAndReturnFallback(`Splice is not a function: ${typeof array.splice}`, fullContext);
    }

    try {
        const result = array.splice(start, deleteCount, ...items);

        trackEvent('splice_success', fullContext, {
            result: {
                removedItems: result.length,
                newArrayLength: array.length,
            },
        });

        return result;
    } catch (err: any) {
        return attemptFallbackSplice(array, start, deleteCount, items, fullContext, err);
    }
};
