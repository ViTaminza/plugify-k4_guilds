{
    global:
        Plugify_*;

        /* s2sdk binding slots (defined by s2sdk_bindings, filled by the plugify module) */
        __s2sdk_*;

        /* hudmenu binding slots (defined by hudmenu_bindings, filled by the plugify module from
           the optional hudmenu plugin's exported GetClientMenuType/SetClientMenuType). Used only
           for the player's own "!menu" choice; the clan menu works without hudmenu. */
        __hudmenu_*;

        /* No exported methods: this plugin is the in-game runtime only. */

    local: *;
};
