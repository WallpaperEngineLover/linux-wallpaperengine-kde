globalThis.__intervals = Object.create(null);
// same shape as scenescript64.dll's: values go through JSON like WE's on-disk store, a location
// other than "global" (or none) means the per-screen store, and setting undefined deletes the key
globalThis.localStorage = globalThis.localStorage || {
  LOCATION_GLOBAL: 'global',
  LOCATION_SCREEN: 'screen',
  __stores: { global: Object.create(null), screen: Object.create(null) },
  __store(location) {
    return String(location).toLowerCase() === 'global' ? this.__stores.global : this.__stores.screen;
  },
  get(key, location) {
    const store = this.__store(location);
    key = String(key);
    return Object.prototype.hasOwnProperty.call(store, key) ? JSON.parse(store[key]) : undefined;
  },
  set(key, value, location) {
    const serialized = value === undefined ? undefined : JSON.stringify(value);
    if (serialized === undefined) {
      this.delete(key, location);
      return;
    }
    this.__store(location)[String(key)] = serialized;
  },
  delete(key, location) { delete this.__store(location)[String(key)]; },
  clear(location) {
    if (String(location).toLowerCase() === 'global') {
      this.__stores.global = Object.create(null);
    } else {
      this.__stores.screen = Object.create(null);
    }
  }
};
globalThis.MediaPlaybackEvent = globalThis.MediaPlaybackEvent || {
  PLAYBACK_STOPPED: 0,
  PLAYBACK_PLAYING: 1,
  PLAYBACK_PAUSED: 2
};
