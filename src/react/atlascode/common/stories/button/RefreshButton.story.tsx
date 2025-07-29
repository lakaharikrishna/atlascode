import { action } from '@storybook/addon-actions';
import * as React from 'react';

import { RefreshButton } from '../../button/RefreshButton';

export default {
    title: 'atlascode/common/button/RefreshButton',
};

export const notLoading = () => {
    return <RefreshButton loading={false} onClick={action('not loading clicked')} />;
};

export const loading = () => {
    return <RefreshButton loading={true} onClick={action('not loading clicked')} />;
};
