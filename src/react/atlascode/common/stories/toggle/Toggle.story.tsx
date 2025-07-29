import { Checkbox, Radio, Switch, Tooltip } from '@material-ui/core';
import * as React from 'react';

import { ToggleWithLabel } from '../../toggle/ToggleWithLabel';
export default {
    title: `atlascode/common/toggle/ToggleWithLabel`,
};

export const switchControl = () => {
    return <ToggleWithLabel label="I'm a switch!" variant="body1" control={<Switch size="small" />} />;
};

export const radioControl = () => {
    return <ToggleWithLabel label="I'm a radio!" variant="body1" control={<Radio size="small" />} />;
};

export const checkboxControl = () => {
    return <ToggleWithLabel label="I'm a checkbox!" variant="body1" control={<Checkbox size="small" />} />;
};

export const switchControlWithTooltip = () => {
    return (
        <ToggleWithLabel
            label="I have a tooltip!"
            variant="body1"
            control={
                <Tooltip title="I'm a tooltip">
                    <Switch size="small" />
                </Tooltip>
            }
        />
    );
};

export const lotsOfSpace = () => {
    return (
        <ToggleWithLabel
            label="I have a tooltip!"
            variant="body1"
            spacing={8}
            control={
                <Tooltip title="I'm a tooltip">
                    <Switch size="small" />
                </Tooltip>
            }
        />
    );
};

export const noSpace = () => {
    return (
        <ToggleWithLabel
            label="I have a tooltip!"
            variant="body1"
            spacing={0}
            control={
                <Tooltip title="I'm a tooltip">
                    <Switch size="small" />
                </Tooltip>
            }
        />
    );
};
