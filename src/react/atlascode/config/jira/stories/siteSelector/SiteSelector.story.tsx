import React, { useCallback, useState } from 'react';

import { SiteSelector } from '../../SiteSelector';
export default {
    title: 'atlascode/config/jira/SiteSelector',
};

interface MySite {
    id: string;
    name: string;
    avatarUrl: string;
    baseUrl: string;
}

const errorSites: MySite[] = [
    {
        id: 'one',
        name: 'one',
        avatarUrl: 'https://avatars3.githubusercontent.com/u/620106?s=80&v=4',
        baseUrl: 'http://one.com',
    },
    {
        id: 'two',
        name: 'two',
        avatarUrl: 'https://me.com/image.jpg',
        baseUrl: 'http://two.com',
    },
];

export const SiteSelectorFallbackImage = () => {
    const [site, setSite] = useState(errorSites[0]);

    const handleSelected = useCallback((site: MySite) => {
        console.log(`selected`, site);
        setSite(site);
    }, []);

    return <SiteSelector sites={errorSites} value={site.id} onSiteChange={handleSelected} />;
};
