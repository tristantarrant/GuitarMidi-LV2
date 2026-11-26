function (event) {
    // GuitarMidi-LV2 MOD level meter.
    //
    // modgui:monitoredOutputs makes mod-host push the "inlevel" and "inpeak"
    // output control ports here: once per port in the "start" event, then as
    // "change" events whenever the value moves. Both are dBFS in [-80, 0]
    // (post-gain, measured by the plugin) - see hardware/moddevices/gui/README.md.
    //
    // The bar is a full-scale gradient on the track (blue < -40 dB,
    // green -40..-15, amber -15..-5, red >= -5); the mask hides the part above
    // the current level, and the peak tick keeps the block peak visible.
    var MIN_DB = -80.0;
    var MAX_DB = 0.0;

    function percent(value) {
        var db = (typeof value === 'number' && !isNaN(value)) ? value : MIN_DB;
        if (db < MIN_DB) {
            db = MIN_DB;
        }
        if (db > MAX_DB) {
            db = MAX_DB;
        }
        return 100.0 * (db - MIN_DB) / (MAX_DB - MIN_DB);
    }

    function set_offset(icon, role, value) {
        icon.find('[mod-role=' + role + ']').css('left', percent(value).toFixed(2) + '%');
    }

    function handle_event(icon, symbol, value) {
        if (symbol === 'inlevel') {
            set_offset(icon, 'meter-mask', value);
        }
        else if (symbol === 'inpeak') {
            set_offset(icon, 'meter-peak', value);
        }
    }

    if (event.type === 'start') {
        var ports = event.ports;
        for (var p in ports) {
            handle_event(event.icon, ports[p].symbol, ports[p].value);
        }
    }
    else if (event.type === 'change') {
        handle_event(event.icon, event.symbol, event.value);
    }
}
