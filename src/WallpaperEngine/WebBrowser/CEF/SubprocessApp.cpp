#include "SubprocessApp.h"
#include "WallpaperEngine/WebBrowser/WebBrowserContext.h"

using namespace WallpaperEngine::WebBrowser::CEF;

SubprocessApp::SubprocessApp (WallpaperEngine::Application::WallpaperApplication& application) :
    m_application (application) { }

void SubprocessApp::OnRegisterCustomSchemes (CefRawPtr<CefSchemeRegistrar> registrar) {
    // One fixed scheme shared by every web wallpaper; the factory resolves the project per
    // request by host (workshop id).
    registrar->AddCustomScheme (
	WPENGINE_SCHEME, CEF_SCHEME_OPTION_STANDARD | CEF_SCHEME_OPTION_SECURE | CEF_SCHEME_OPTION_FETCH_ENABLED
    );
}

CefRefPtr<CefRenderProcessHandler> SubprocessApp::GetRenderProcessHandler () { return this; }

void SubprocessApp::OnContextCreated (
    CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame, CefRefPtr<CefV8Context> context
) {
    // The parts of Wallpaper Engine's web runtime that aren't plain user properties. The host process (PageBridge)
    // calls the __lwe* functions, everything else is what pages call or register on. Has to exist before the first
    // page script: a missing wallpaperRegister*Listener throws and takes the rest of the page's setup down with it.
    static const char* script = R"JS(
(function () {
    var reported = new WeakSet();

    // off-screen alert() is a silent no-op, and pages routinely report their setup errors through it
    window.alert = function (message) {
	console.error('[lwe] alert(): ' + message);
    };

    var safeCall = function (callback, argument) {
	try { callback(argument); } catch (e) {
	    if (!reported.has(callback)) { reported.add(callback); console.error(e); }
	}
    };

    var audioListeners = [];
    if (typeof window.wallpaperRegisterAudioListener !== 'function') {
	window.wallpaperRegisterAudioListener = function (callback) { audioListeners.push(callback); };
    }
    // one array per audio frame: 64 left bands then 64 right bands
    window.__lweAudio = function (frame) {
	for (var i = 0; i < audioListeners.length; i++) safeCall(audioListeners[i], frame);
    };

    // a listener that registers after the first event still gets the current state, like it does on Windows
    var mediaListeners = {};
    var mediaLast = {};
    var mediaKinds = { properties: 'Properties', thumbnail: 'Thumbnail', timeline: 'Timeline', playback: 'Playback', status: 'Status' };
    Object.keys(mediaKinds).forEach(function (key) {
	var kind = mediaKinds[key];
	var name = 'wallpaperRegisterMedia' + kind + 'Listener';
	if (typeof window[name] === 'function') return;
	window[name] = function (callback) {
	    (mediaListeners[key] = mediaListeners[key] || []).push(callback);
	    if (mediaLast[key]) safeCall(callback, mediaLast[key]);
	};
    });
    window.__lweMedia = function (events) {
	Object.keys(events).forEach(function (key) {
	    if (!mediaKinds[key]) return;
	    mediaLast[key] = events[key];
	    (mediaListeners[key] || []).forEach(function (callback) { safeCall(callback, events[key]); });
	});
    };

    // A page may only define window.wallpaperPropertyListener some time after it finished loading, so keep the
    // current state and give it to whichever listener object shows up. Properties wait until the host has reported
    // every directory (or a few seconds have passed), slideshow pages need the file list before the properties.
    var lastProperties = null;
    var directoryFiles = {};
    var deliveredTo = null;
    var directoriesReady = false;
    var startedAt = Date.now();
    // what the page did with what it was given, for when a wallpaper shows nothing
    var describe = function (listener) {
	var interesting = {};
	Object.keys(lastProperties).forEach(function (key) {
	    if (/wallpapermode|directory|imageswitch|imagedisplay|^image$|random$|^weather/i.test(key)) interesting[key] = lastProperties[key].value;
	});
	console.warn('[lwe] page listener has: ' + Object.keys(listener).join(', '));
	console.warn('[lwe] properties of interest: ' + JSON.stringify(interesting));
	setTimeout(function () {
	    var visible = function (list) {
		return Array.prototype.slice.call(list).map(function (element) { return (element.currentSrc || element.src || '').slice(-60); }).filter(Boolean);
	    };
	    var style = getComputedStyle(document.body);
	    console.warn('[lwe] page state after 6s: ' + JSON.stringify({
		imgs: visible(document.images).slice(0, 5),
		videos: visible(document.querySelectorAll('video')).slice(0, 3),
		canvases: document.querySelectorAll('canvas').length,
		bodyBackground: style.backgroundImage.slice(0, 120),
		bgImgVariable: style.getPropertyValue('--bg-img').slice(0, 120),
		files: Object.keys(directoryFiles).map(function (name) { return name + ':' + directoryFiles[name].length; })
	    }));
	}, 6000);
    };
    var deliver = function () {
	var listener = window.wallpaperPropertyListener;
	if (!listener || listener === deliveredTo || lastProperties === null) return;
	if (typeof listener.applyUserProperties !== 'function') return;
	if (!directoriesReady && Date.now() - startedAt < 3000) return;

	deliveredTo = listener;
	describe(listener);

	// Pages on the older jquery.backstretch slideshow only show a photo when their directory property is applied
	// (their file callbacks just store the list), but they set their default background while the list is still
	// empty. Those get the properties first, then the files, then the directory properties once more, on their own,
	// the way a changed property arrives. Everything else gets the files first and the properties after.
	var directories = Object.keys(directoryFiles).filter(function (name) { return directoryFiles[name].length && lastProperties[name]; });
	var slideshowFirst = directories.length > 0 && !!(window.jQuery && window.jQuery.fn && window.jQuery.fn.backstretch);

	if (slideshowFirst) safeCall(function (properties) { listener.applyUserProperties(properties); }, lastProperties);

	Object.keys(directoryFiles).forEach(function (name) {
	    if (!directoryFiles[name].length) return;
	    if (typeof listener.userDirectoryFilesAddedOrChanged !== 'function') {
		console.warn('[lwe] the page has no userDirectoryFilesAddedOrChanged, ' + directoryFiles[name].length + ' files of ' + name + ' were not delivered');
		return;
	    }
	    console.warn('[lwe] gave ' + directoryFiles[name].length + ' files of ' + name + ' to the page');
	    safeCall(function (files) { listener.userDirectoryFilesAddedOrChanged(name, files); }, directoryFiles[name].slice());
	});

	if (!slideshowFirst) {
	    safeCall(function (properties) { listener.applyUserProperties(properties); }, lastProperties);
	    return;
	}

	var changed = {};
	directories.forEach(function (name) { changed[name] = lastProperties[name]; });
	// a page written against whole property objects may trip over a partial one after it did what it needed to
	try { listener.applyUserProperties(changed); } catch (e) { console.warn('[lwe] the page threw on the directory update: ' + e); }
    };
    window.__lweDirectoriesReady = function () {
	directoriesReady = true;
	deliver();
    };
    // Slideshow pages built on jquery.backstretch always crop to fill: their own fill/stretch/fit/center option only
    // touches the single image mode, so it does nothing while the slideshow runs. The option is honored for the
    // slideshow too, through the image's own sizing (1 fill, 2 stretch, 3 fit, 5 center, tiling has no equivalent).
    var fitStyle = null;
    var applyImageFit = function (properties) {
	var option = properties.imagedisplaystlye;
	var fits = { 2: 'fill', 3: 'contain', 5: 'none' };
	var fit = option && fits[Number(option.value)];
	if (option) {
	    console.warn('[lwe] imagedisplaystlye ' + JSON.stringify(option.value) + ' -> ' + (fit ? 'object-fit ' + fit : 'page default'));
	    setTimeout(function () {
		var images = document.querySelectorAll('.backstretch img');
		var last = images[images.length - 1];
		var box = last && last.getBoundingClientRect();
		console.warn('[lwe] slideshow images: ' + images.length + (last ? ', last ' + Math.round(box.width) + 'x' + Math.round(box.height)
		    + ' at ' + Math.round(box.left) + ',' + Math.round(box.top) + ', object-fit ' + getComputedStyle(last).objectFit
		    + ', natural ' + last.naturalWidth + 'x' + last.naturalHeight + ', viewport ' + innerWidth + 'x' + innerHeight : ''));
	    }, 8000);
	}
	if (!fit && !fitStyle) return;
	if (!fitStyle) {
	    fitStyle = document.createElement('style');
	    (document.head || document.documentElement).appendChild(fitStyle);
	}
	fitStyle.textContent = fit ? '.backstretch img { width: 100% !important; height: 100% !important; left: 0 !important; '
	    + 'top: 0 !important; max-width: none !important; max-height: none !important; object-fit: ' + fit + ' !important; }' : '';
    };
    window.__lweProperties = function (properties) {
	applyImageFit(properties);
	lastProperties = properties;
	deliveredTo = null;
	deliver();
    };
    setInterval(deliver, 250);
    setTimeout(function () {
	if (lastProperties !== null && !deliveredTo) console.warn('[lwe] the page did not define wallpaperPropertyListener.applyUserProperties within 10 seconds');
    }, 10000);

    // directory properties, see PageBridge::scanDirectories
    window.__lweDirectory = function (kind, name, files) {
	var known = directoryFiles[name] || [];
	if (kind === 'added') {
	    files.forEach(function (file) { if (known.indexOf(file) < 0) known.push(file); });
	} else {
	    known = known.filter(function (file) { return files.indexOf(file) < 0; });
	}
	directoryFiles[name] = known;

	// a listener that has not been given the initial state yet gets these as part of it, see deliver()
	var listener = window.wallpaperPropertyListener;
	var method = kind === 'added' ? 'userDirectoryFilesAddedOrChanged' : 'userDirectoryFilesRemoved';
	if (listener && listener === deliveredTo) {
	    if (typeof listener[method] !== 'function') {
		console.warn('[lwe] the page has no ' + method + ', ' + files.length + ' files of ' + name + ' were not delivered');
		return;
	    }
	    console.warn('[lwe] gave ' + files.length + ' files (' + kind + ') of ' + name + ' to the page');
	    safeCall(function (files) { listener[method](name, files); }, files);
	}
    };
    if (typeof window.wallpaperRequestRandomFileForProperty !== 'function') {
	window.wallpaperRequestRandomFileForProperty = function (name, callback) {
	    var known = directoryFiles[name] || [];
	    callback(name, known.length ? known[Math.floor(Math.random() * known.length)] : null);
	};
    }

    // Pages turn file and directory properties into "file:///" + path URLs, which this origin can't load, so point
    // them at the wallpaper's own scheme (WPSchemeHandler serves them from disk)
    var origin = location.origin;
    if (!origin || origin === 'null') return;

    var fileUrl = /file:\/\/\/([^'")]*)/g;
    var encodePath = function (path) {
	// pages hand these over both raw and already escaped, normalize to one form
	return path.replace(/^\/+/, '').split('/').map(function (segment) {
	    try { segment = decodeURIComponent(segment); } catch (e) {}
	    return encodeURIComponent(segment);
	}).join('/');
    };
    // what the page sets as sources, to see how a page that shows nothing goes about showing things
    var sourcesTraced = 0;
    var traceSource = function (kind, value) {
	if (sourcesTraced >= 40 || typeof value !== 'string' || !value || value.indexOf('data:') === 0) return;
	sourcesTraced++;
	console.warn('[lwe] page sets ' + kind + ': ' + value.slice(0, 140));
    };
    var traced = 0;
    var rewrite = function (value) {
	if (typeof value !== 'string' || value.indexOf('file:///') === -1) return value;
	return value.replace(fileUrl, function (match, path) {
	    // pages build these from not-yet-set properties too ("file:///" + "" or + {})
	    if (!path || path.indexOf('[object') === 0) return match;
	    if (traced++ < 10) console.warn('[lwe] file URL for the page: /' + path);
	    return origin + '/__lwe_file__/' + encodePath(path);
	});
    };
    // Chromium reports failed subresource loads nowhere the host can see, so say which ones failed
    var failures = 0;
    window.addEventListener('error', function (event) {
	var target = event.target;
	if (!target || target === window) return;
	var source = target.currentSrc || target.src || target.href;
	// an empty src attribute resolves to the page itself, pages reset their media elements that way all the time
	if (!source || source === location.href || failures++ >= 20) return;
	console.warn('[lwe] failed to load ' + target.tagName.toLowerCase() + ' ' + source);
    }, true);
    var wrapSetter = function (target, property, wrap, label) {
	var descriptor = target && Object.getOwnPropertyDescriptor(target, property);
	if (!descriptor || !descriptor.set) return;
	Object.defineProperty(target, property, {
	    configurable: true,
	    enumerable: descriptor.enumerable,
	    get: descriptor.get,
	    set: function (value) {
		if (label) traceSource(label + '.' + property, value);
		descriptor.set.call(this, wrap(value));
	    },
	});
    };

    var setProperty = CSSStyleDeclaration.prototype.setProperty;
    CSSStyleDeclaration.prototype.setProperty = function (name, value, priority) {
	if (typeof value === 'string' && value.indexOf('url(') !== -1) traceSource('css ' + name, value);
	return setProperty.call(this, name, rewrite(value), priority);
    };
    ['backgroundImage', 'background', 'maskImage', 'webkitMaskImage', 'borderImageSource', 'listStyleImage', 'content', 'cssText']
	.forEach(function (property) { wrapSetter(CSSStyleDeclaration.prototype, property, rewrite); });

    [['img', HTMLImageElement], ['media', HTMLMediaElement], ['source', HTMLSourceElement], ['iframe', HTMLIFrameElement]].forEach(function (entry) {
	wrapSetter(entry[1].prototype, 'src', rewrite, entry[0]);
    });
    wrapSetter(HTMLVideoElement.prototype, 'poster', rewrite);

    var setAttribute = Element.prototype.setAttribute;
    Element.prototype.setAttribute = function (name, value) {
	var lower = String(name).toLowerCase();
	if (lower === 'src' || lower === 'poster' || lower === 'srcset' || lower === 'style') {
	    traceSource('attribute ' + lower, value);
	    value = rewrite(value);
	}
	return setAttribute.call(this, name, value);
    };

    // camelCase style properties (style.backgroundImage) bypass the setters above, so fix the style attribute they
    // end up in. Uses the native setAttribute so the fix isn't traced or rewritten again.
    var fixStyleAttribute = function (element) {
	var css = element.getAttribute && element.getAttribute('style');
	if (!css || css.indexOf('file:///') === -1) return;

	// even an identical value queues another record for this observer, and a URL rewrite() leaves alone (a page's
	// "file:///" + "" before its properties arrived) would then loop forever
	var fixed = rewrite(css);
	if (fixed !== css) setAttribute.call(element, 'style', fixed);
    };
    new MutationObserver(function (records) {
	records.forEach(function (record) { fixStyleAttribute(record.target); });
    }).observe(document, { attributes: true, attributeFilter: ['style'], subtree: true });

    var nativeFetch = window.fetch;
    if (nativeFetch) {
	window.fetch = function (input, init) {
	    traceSource('fetch', typeof input === 'string' ? input : input && input.url);
	    return nativeFetch.call(this, typeof input === 'string' ? rewrite(input) : input, init);
	};
    }
    var xhrOpen = XMLHttpRequest.prototype.open;
    XMLHttpRequest.prototype.open = function (method, url) {
	var args = Array.prototype.slice.call(arguments);
	traceSource('xhr', String(url));
	args[1] = rewrite(String(url));
	return xhrOpen.apply(this, args);
    };
    var NativeFontFace = window.FontFace;
    if (NativeFontFace) {
	window.FontFace = function (family, source, descriptors) {
	    return new NativeFontFace(family, typeof source === 'string' ? rewrite(source) : source, descriptors);
	};
	window.FontFace.prototype = NativeFontFace.prototype;
    }
})();
)JS";

    frame->ExecuteJavaScript (script, frame->GetURL (), 0);
}

const WallpaperEngine::Application::WallpaperApplication& SubprocessApp::getApplication () const {
    return this->m_application;
}