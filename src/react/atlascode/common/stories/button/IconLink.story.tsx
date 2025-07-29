import InsertEmoticonIcon from '@material-ui/icons/InsertEmoticon';
import * as React from 'react';

import { IconLink } from '../../button/IconLink';

export default {
    title: `atlascode/common/button/IconLink`,
};

export const noIcon = () => {
    return <IconLink>link test</IconLink>;
};

export const startIcon = () => {
    return <IconLink startIcon={<InsertEmoticonIcon />}>link test</IconLink>;
};

export const endIcon = () => {
    return <IconLink endIcon={<InsertEmoticonIcon />}>link test</IconLink>;
};

export const startAndEndIcon = () => {
    return (
        <IconLink startIcon={<InsertEmoticonIcon />} endIcon={<InsertEmoticonIcon />}>
            link test
        </IconLink>
    );
};
