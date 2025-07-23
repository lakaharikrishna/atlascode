import { Container } from '../container';
import { safeSplice } from './safeSplice';

jest.mock('../container', () => ({
    Container: {
        analyticsClient: {
            sendTrackEvent: jest.fn(),
        },
    },
}));

describe('safeSplice', () => {
    beforeEach(() => {
        jest.clearAllMocks();
    });

    it('should successfully splice array and log success', () => {
        const array = [1, 2, 3, 4, 5];
        const result = safeSplice(array, 1, 2, {
            file: 'test.ts',
            function: 'testFunction',
        });

        expect(result).toEqual([2, 3]);
        expect(array).toEqual([1, 4, 5]);

        expect(Container.analyticsClient.sendTrackEvent).toHaveBeenCalledWith({
            trackEvent: {
                action: 'splice_success',
                actionSubject: 'array_operation',
                attributes: {
                    context: {
                        file: 'test.ts',
                        function: 'testFunction',
                        arrayType: 'object',
                        arrayLength: 5,
                        startIndex: 1,
                        deleteCount: 2,
                        timestamp: expect.any(String),
                    },
                    result: {
                        removedItems: 2,
                        newArrayLength: 3,
                    },
                },
            },
        });
    });

    it('should handle modified Array.prototype and log error', () => {
        const originalSplice = Array.prototype.splice;

        try {
            Array.prototype.splice = undefined as any;

            const array = [1, 2, 3];
            const result = safeSplice(array, 1, 1, {
                file: 'test.ts',
                function: 'testFunction',
            });

            expect(result).toEqual([]);

            expect(Container.analyticsClient.sendTrackEvent).toHaveBeenCalledWith({
                trackEvent: {
                    action: 'splice_error',
                    actionSubject: 'array_operation',
                    attributes: {
                        error: {
                            message: expect.stringContaining('Splice is not a function'),
                            stack: expect.any(String),
                        },
                        context: {
                            file: 'test.ts',
                            function: 'testFunction',
                            arrayType: 'object',
                            arrayLength: 3,
                            startIndex: 1,
                            deleteCount: 1,
                            timestamp: expect.any(String),
                        },
                    },
                },
            });
        } finally {
            Array.prototype.splice = originalSplice;
        }
    });

    it('should log error if input is not an array', () => {
        const result = safeSplice({} as any, 0, 1, {
            file: 'test.ts',
            function: 'testFunction',
        });

        expect(result).toEqual([]);

        expect(Container.analyticsClient.sendTrackEvent).toHaveBeenCalledWith(
            expect.objectContaining({
                trackEvent: expect.objectContaining({
                    action: 'splice_error',
                    attributes: expect.objectContaining({
                        error: expect.objectContaining({
                            message: expect.stringContaining('Not an array'),
                        }),
                    }),
                }),
            }),
        );
    });
});
