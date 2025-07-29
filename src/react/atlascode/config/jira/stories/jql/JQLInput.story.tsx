import { AuthorizationProvider, JiraServerClient, TransportFactory } from '@atlassianlabs/jira-pi-client';
import { Grid, makeStyles, TextField } from '@material-ui/core';
import axios, { CancelToken } from 'axios';
import React, { useMemo, useState } from 'react';

import { JQLInput } from '../../jql/JQLInput/JQLInput';
import { FieldSuggestionFetcher, Suggestion } from '../../jql/jqlTypes';
import restdata from './__fixtures__/autocompletedata.json';

export default {
    title: 'atlascode/config/jira/jql/JQLInput',
};

const useStyles = makeStyles({
    '@global': {
        '.MuiAutocomplete-option[data-focus="true"]': {
            'background-color': '#e5e5e5',
        },
    },
});

export const JqlInputMockData = () => {
    useStyles();
    const fetcher: FieldSuggestionFetcher = async (
        fieldName: string,
        userInput: string,
        predicateName?: string,
    ): Promise<Suggestion[]> => {
        return new Promise((resolve) => {
            setTimeout(resolve, 2000, [
                { value: 'VSCODE', displayName: 'VSCODE (VSCode)' },
                { value: 'VTC', displayName: 'VTC (Classic)' },
                { value: 'VTS', displayName: 'VTS (Server)' },
                { value: 'VTN', displayName: 'VTN (Next Gen)' },
            ]);
        });
    };

    return (
        <JQLInput
            label="Enter JQL"
            variant="outlined"
            fullWidth
            jqlAutocompleteRestData={restdata}
            suggestionFetcher={fetcher}
        />
    );
};

export const JqlInputTextArea = () => {
    useStyles();
    const fetcher: FieldSuggestionFetcher = async (
        fieldName: string,
        userInput: string,
        predicateName?: string,
    ): Promise<Suggestion[]> => {
        return new Promise((resolve) => {
            setTimeout(resolve, 2000, [
                { value: 'VSCODE', displayName: 'VSCODE (VSCode)' },
                { value: 'VTC', displayName: 'VTC (Classic)' },
                { value: 'VTS', displayName: 'VTS (Server)' },
                { value: 'VTN', displayName: 'VTN (Next Gen)' },
            ]);
        });
    };

    return (
        <JQLInput
            label="Enter JQL"
            variant="outlined"
            fullWidth
            multiline
            rows={10}
            jqlAutocompleteRestData={restdata}
            suggestionFetcher={fetcher}
        />
    );
};

export const JqlInputRealData: React.FunctionComponent<{}> = ({}) => {
    useStyles();
    const [server, setServer] = useState('https://jira.pi-server.tk');
    const [name, setName] = useState('');
    const [pword, setPword] = useState('');

    const handleNameChange = (event: React.ChangeEvent<HTMLInputElement>) => {
        setName(event.target.value);
    };
    const handlePwordChange = (event: React.ChangeEvent<HTMLInputElement>) => {
        setPword(event.target.value);
    };
    const handleServerChange = (event: React.ChangeEvent<HTMLInputElement>) => {
        let serverUrl = event.target.value;

        if (serverUrl.endsWith('/rest') || serverUrl.endsWith('/')) {
            serverUrl = serverUrl.substring(0, serverUrl.lastIndexOf('/'));
        }

        setServer(serverUrl);
    };

    const jiraServerAuthProvider = (username: string, password: string): AuthorizationProvider => {
        const basicAuth = Buffer.from(`${username}:${password}`).toString('base64');
        return (method: string, url: string) => {
            return Promise.resolve(`Basic ${basicAuth}`);
        };
    };

    const jiraTransportFactory: TransportFactory = () => {
        const transport = axios.create({
            timeout: 30 * 1000,
            headers: {},
        });

        return transport;
    };
    //https://jira.pi-server.tk
    const fetcher: FieldSuggestionFetcher = useMemo(
        () =>
            async (
                fieldName: string,
                userInput: string,
                predicateName?: string,
                abortSignal?: AbortSignal,
            ): Promise<Suggestion[]> => {
                let cancelToken: CancelToken | undefined = undefined;

                if (abortSignal) {
                    const cancelSignal = axios.CancelToken.source();
                    cancelToken = cancelSignal.token;
                    abortSignal.onabort = () => cancelSignal.cancel('suggestion fetch aborted');
                }
                const clientRef = new JiraServerClient(
                    { baseApiUrl: server + '/rest', isCloud: false },
                    jiraTransportFactory,
                    jiraServerAuthProvider(name, pword),
                );
                return clientRef.getFieldAutoCompleteSuggestions(fieldName, userInput, predicateName, cancelToken);
            },
        [name, pword, server],
    );

    return (
        <Grid container direction="column" spacing={2}>
            <Grid item>
                <TextField
                    defaultValue={server}
                    required
                    autoFocus
                    autoComplete="off"
                    id="baseUrl"
                    name="baseUrl"
                    label="Base URL"
                    helperText="URL to jira server instance"
                    onChange={handleServerChange}
                    style={{ width: 400 }}
                />
            </Grid>
            <Grid item>
                <TextField
                    required
                    margin="dense"
                    id="username"
                    name="username"
                    label="Username"
                    onChange={handleNameChange}
                    style={{ width: 300 }}
                />
            </Grid>
            <Grid item>
                <TextField
                    required
                    margin="dense"
                    id="password"
                    name="password"
                    label="Password"
                    type="password"
                    onChange={handlePwordChange}
                    style={{ width: 300 }}
                />
            </Grid>
            <Grid item>
                <JQLInput
                    label="Enter JQL"
                    //variant="outlined"
                    fullWidth
                    disabled={name.length < 1 || pword.length < 1 || server.length < 10}
                    jqlAutocompleteRestData={restdata}
                    suggestionFetcher={fetcher}
                />
            </Grid>
        </Grid>
    );
};

export const JqlInputControlled = () => {
    useStyles();
    const fetcher: FieldSuggestionFetcher = async (
        fieldName: string,
        userInput: string,
        predicateName?: string,
    ): Promise<Suggestion[]> => {
        return new Promise((resolve) => {
            setTimeout(resolve, 2000, [
                { value: 'VSCODE', displayName: 'VSCODE (VSCode)' },
                { value: 'VTC', displayName: 'VTC (Classic)' },
                { value: 'VTS', displayName: 'VTS (Server)' },
                { value: 'VTN', displayName: 'VTN (Next Gen)' },
            ]);
        });
    };

    return (
        <JQLInput
            label="Enter JQL"
            variant="standard"
            defaultValue="project = VTS"
            onChange={(event: React.ChangeEvent<HTMLInputElement>) => console.log(`newVal: ${event.target.value}`)}
            fullWidth
            jqlAutocompleteRestData={restdata}
            suggestionFetcher={fetcher}
        />
    );
};
