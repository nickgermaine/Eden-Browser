#pragma once

namespace eden::engine {

    inline constexpr const char *mediaActivityScript = R"JS((function(report){
'use strict';
const devices = navigator.mediaDevices;
if (!devices || typeof MediaStreamTrack === 'undefined') { return; }
const tracks = new Map();
const trackPrototype = MediaStreamTrack.prototype;
const nativeStop = trackPrototype.stop;
const nativeClone = trackPrototype.clone;
const nativeSettings = trackPrototype.getSettings;
const nativeState = Object.getOwnPropertyDescriptor(trackPrototype, 'readyState').get;
const nativeKind = Object.getOwnPropertyDescriptor(trackPrototype, 'kind').get;
const addListener = EventTarget.prototype.addEventListener;
const nativeThen = Promise.prototype.then;
const streamTracks = MediaStream.prototype.getTracks;
const streamClone = MediaStream.prototype.clone;
let last = -1;
let hidden = false;
const publish = function(){
    let value = 0;
    for (const [track, kind] of tracks) {
        if (nativeState.call(track) === 'ended') { tracks.delete(track); }
        else { value |= kind; }
    }
    if (hidden) { value = 0; }
    if (value !== last) { last = value; report(value); }
};
const remember = function(track, kind){
    if (nativeState.call(track) === 'ended' || tracks.has(track)) { return; }
    tracks.set(track, kind);
    addListener.call(track, 'ended', publish);
};
const wrap = function(name, display){
    const original = devices[name];
    if (typeof original !== 'function') { return; }
    Object.defineProperty(devices, name, {configurable:true, writable:true, value:function(options){
        const legacy = options && options.video && options.video.mandatory;
        const desktop = display || Boolean(legacy && legacy.chromeMediaSource === 'desktop');
        return nativeThen.call(original.call(devices, options), function(stream){
            for (const track of streamTracks.call(stream)) {
                const settings = nativeSettings.call(track);
                const sharing = desktop || Boolean(settings.displaySurface);
                remember(track, nativeKind.call(track) === 'video' ? (sharing ? 4 : 1) : (sharing ? 8 : 2));
            }
            publish();
            return stream;
        });
    }});
};
trackPrototype.stop = function(){
    const result = nativeStop.call(this);
    publish();
    return result;
};
trackPrototype.clone = function(){
    const clone = nativeClone.call(this);
    if (tracks.has(this)) { remember(clone, tracks.get(this)); publish(); }
    return clone;
};
MediaStream.prototype.clone = function(){
    const clone = streamClone.call(this);
    const originals = streamTracks.call(this);
    const copies = streamTracks.call(clone);
    for (let index = 0; index < originals.length; ++index) {
        if (tracks.has(originals[index])) { remember(copies[index], tracks.get(originals[index])); }
    }
    publish();
    return clone;
};
wrap('getUserMedia', false);
wrap('getDisplayMedia', true);
addListener.call(window, 'pagehide', function(){ hidden = true; publish(); });
addListener.call(window, 'pageshow', function(){ hidden = false; publish(); });
publish();
})
)JS";

}
