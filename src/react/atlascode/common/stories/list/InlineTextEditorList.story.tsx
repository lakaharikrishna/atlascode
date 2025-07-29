import { Box, Button, Grid, GridJustification, MenuItem, TextField } from '@material-ui/core';
import * as React from 'react';
import { useCallback, useState } from 'react';

import { InlineTextEditorList } from '../../list/InlineTextEditorList';
import { ToggleWithLabel } from '../../toggle/ToggleWithLabel';

export default {
    title: 'atlascode/common/list/InlineTextEditorList',
};

export const empty = () => {
    return (
        <InlineTextEditorList options={[]} addOptionButtonContent="Add Item" disabled={false} inputLabel="Item Text" />
    );
};

export const emptyCustomComponent = () => {
    return (
        <InlineTextEditorList
            options={[]}
            addOptionButtonContent="Add Item"
            disabled={false}
            inputLabel="Item Text"
            emptyComponent={<div>Ain't no stuff in heres</div>}
        />
    );
};

export const initialData = () => {
    return (
        <InlineTextEditorList
            options={['x', 'z', 'a', 'b']}
            addOptionButtonContent="Add Item"
            disabled={false}
            inputLabel="Item Text"
        />
    );
};

export const customButtons = () => {
    return (
        <InlineTextEditorList options={[]} addOptionButtonContent="Add Item" disabled={false} inputLabel="Item Text">
            <Button variant="contained">Custom 1</Button>
            <Button variant="contained">Custom 2</Button>
        </InlineTextEditorList>
    );
};

export const AlignButtons = () => {
    const [justifyButtons, setJustifyButtons] = useState<GridJustification | undefined>('flex-start');
    const handleChange = useCallback((event: React.ChangeEvent<HTMLInputElement>) => {
        if (event.target.value === 'undefined') {
            setJustifyButtons(undefined);
        } else {
            setJustifyButtons(event.target.value as GridJustification);
        }
    }, []);

    const [reverseButtons, setReverseButtons] = useState<boolean>(false);
    const handleReverse = useCallback((event: React.ChangeEvent<HTMLInputElement>) => {
        setReverseButtons(event.target.checked);
    }, []);

    return (
        <Grid container direction="column">
            <Grid item>
                <Box width="300px">
                    <TextField fullWidth select label="Justify Buttons" value={justifyButtons} onChange={handleChange}>
                        <MenuItem key="flex-start" value="flex-start">
                            flex-start
                        </MenuItem>
                        <MenuItem key="center" value="center">
                            center
                        </MenuItem>
                        <MenuItem key="flex-end" value="flex-end">
                            flex-end
                        </MenuItem>
                        <MenuItem key="space-between" value="space-between">
                            space-between
                        </MenuItem>
                        <MenuItem key="space-around" value="space-around">
                            space-around
                        </MenuItem>
                        <MenuItem key="space-evenly" value="space-evenly">
                            space-evenly
                        </MenuItem>
                        <MenuItem key="undefined" value="undefined">
                            undefined
                        </MenuItem>
                    </TextField>
                </Box>
            </Grid>
            <Grid item>
                <ToggleWithLabel
                    label="Reverse Buttons"
                    color="primary"
                    size="small"
                    checked={reverseButtons}
                    value="reversed"
                    onChange={handleReverse}
                />
            </Grid>
            <Grid item>
                <InlineTextEditorList
                    options={[]}
                    addOptionButtonContent="Add Item"
                    disabled={false}
                    justifyButtons={justifyButtons}
                    reverseButtons={reverseButtons}
                    inputLabel="Item Text"
                >
                    <Button variant="contained">Custom 1</Button>
                    <Button variant="contained">Custom 2</Button>
                </InlineTextEditorList>
            </Grid>
        </Grid>
    );
};
