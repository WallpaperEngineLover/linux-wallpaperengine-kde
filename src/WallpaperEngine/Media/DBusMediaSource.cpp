#include "DBusMediaSource.h"

#include "WallpaperEngine/Data/Utils/FloatCompare.h"
#include "WallpaperEngine/Data/Utils/ScopeGuard.h"
#include "WallpaperEngine/Desktop/SessionBus.h"
#include "WallpaperEngine/Logging/Log.h"

#include <thread>

using namespace WallpaperEngine::Media;

static DBusHandlerResult dbus_message_filter (DBusConnection* connection, DBusMessage* message, void* user_data) {
    const auto mediaSource = static_cast<DBusMediaSource*> (user_data);

    if (!dbus_message_is_signal (message, "org.freedesktop.DBus.Properties", "PropertiesChanged")) {
	return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
    }

    if (mediaSource->discarding ()) {
	mediaSource->noteMissed ();
	return DBUS_HANDLER_RESULT_HANDLED;
    }

    const char* interface = nullptr;
    DBusMessageIter iter;

    dbus_message_iter_init (message, &iter);
    dbus_message_iter_get_basic (&iter, &interface);

    std::string iface = interface ?: "";

    if (iface != "org.mpris.MediaPlayer2.Player") {
	return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
    }

    DBusMessageIter changed;

    dbus_message_iter_next (&iter);
    dbus_message_iter_recurse (&iter, &changed);

    // signals come from every MPRIS player on the bus: only the followed one updates the track, another one takes
    // over only by starting to play, idle players (a browser with nothing loaded) are ignored
    const char* sender = dbus_message_get_sender (message);

    if (!mediaSource->isCurrentPlayer (sender)) {
	DBusMessageIter scan = changed;
	bool startedPlaying = false;

	while (dbus_message_iter_get_arg_type (&scan) == DBUS_TYPE_DICT_ENTRY) {
	    DBusMessageIter entry;
	    dbus_message_iter_recurse (&scan, &entry);

	    const char* key = nullptr;
	    dbus_message_iter_get_basic (&entry, &key);

	    if (key != nullptr && std::string_view (key) == "PlaybackStatus") {
		DBusMessageIter value;
		dbus_message_iter_next (&entry);
		dbus_message_iter_recurse (&entry, &value);

		const char* status = nullptr;
		dbus_message_iter_get_basic (&value, &status);
		startedPlaying = status != nullptr && std::string_view (status) == "Playing";
	    }

	    dbus_message_iter_next (&scan);
	}

	if (startedPlaying && sender != nullptr) {
	    mediaSource->switchPlayer (sender);
	}

	return DBUS_HANDLER_RESULT_HANDLED;
    }

    while (dbus_message_iter_get_arg_type (&changed) == DBUS_TYPE_DICT_ENTRY) {
	DBusMessageIter entry;
	dbus_message_iter_recurse (&changed, &entry);

	const char* key = nullptr;
	dbus_message_iter_get_basic (&entry, &key);

	std::string keyStr = key ?: "";

	DBusMessageIter value;
	dbus_message_iter_next (&entry);
	dbus_message_iter_recurse (&entry, &value);

	if (keyStr == "Metadata") {
	    mediaSource->parseMetadata (value);
	} else if (keyStr == "PlaybackStatus") {
	    mediaSource->parsePlaybackStatus (value, dbus_message_get_sender (message));
	}

	dbus_message_iter_next (&changed);
    }

    return DBUS_HANDLER_RESULT_HANDLED;
}

namespace {
constexpr std::chrono::seconds RECONNECT_INTERVAL (5);
}

DBusMediaSource::DBusMediaSource (std::chrono::milliseconds updateInterval) : MediaSource (updateInterval) {
    if (!this->connect ()) {
	sLog.exception ("Could not connect to the DBus session bus");
    }
}

DBusMediaSource::~DBusMediaSource () {
    if (this->m_pending) {
	this->m_pending->abandoned = true;
	Desktop::closeBus (this->m_pending->bus.exchange (nullptr));
    }

    this->disconnect ();
}

bool DBusMediaSource::connect () {
    DBusConnection* connection = Desktop::privateBus (DBUS_BUS_SESSION);

    if (connection == nullptr) {
	sLog.debug ("Media: session bus connection failed");
	return false;
    }

    this->attach (connection);
    return true;
}

void DBusMediaSource::attach (DBusConnection* connection) {
    dbus_connection_add_filter (connection, dbus_message_filter, this, nullptr);
    dbus_bus_add_match (
	connection,
	"type='signal',interface='org.freedesktop.DBus.Properties',member='PropertiesChanged',"
	"path='/org/mpris/MediaPlayer2',arg0='org.mpris.MediaPlayer2.Player'",
	nullptr
    );
    dbus_connection_flush (connection);

    this->m_connection = connection;
}

void DBusMediaSource::disconnect () {
    if (this->m_connection == nullptr) {
	return;
    }

    dbus_connection_remove_filter (this->m_connection, dbus_message_filter, this);
    dbus_connection_close (this->m_connection);
    dbus_connection_unref (this->m_connection);
    this->m_connection = nullptr;
}

bool DBusMediaSource::isConnected () const {
    return this->m_connection != nullptr && dbus_connection_get_is_connected (this->m_connection);
}

void DBusMediaSource::handleLoss () {
    sLog.error ("Media: the session bus dropped the connection, trying again every ", RECONNECT_INTERVAL.count (), "s");

    this->m_currentPlayer.reset ();
    this->disconnect ();
    this->m_nextReconnect = std::chrono::steady_clock::now () + RECONNECT_INTERVAL;
}

void DBusMediaSource::tryReconnect () {
    if (this->m_connection != nullptr) {
	return;
    }

    if (this->m_pending) {
	if (!this->m_pending->done) {
	    return;
	}

	DBusConnection* connection = this->m_pending->bus.exchange (nullptr);
	this->m_pending.reset ();

	if (connection != nullptr) {
	    this->attach (connection);
	    sLog.out ("Media: reconnected to the session bus");
	} else {
	    this->m_nextReconnect = std::chrono::steady_clock::now () + RECONNECT_INTERVAL;
	}

	return;
    }

    if (std::chrono::steady_clock::now () < this->m_nextReconnect) {
	return;
    }

    this->m_pending = std::make_shared<PendingConnect> ();

    std::thread ([pending = this->m_pending] {
	DBusConnection* connection = Desktop::privateBus (DBUS_BUS_SESSION);
	pending->bus = connection;

	if (pending->abandoned) {
	    Desktop::closeBus (pending->bus.exchange (nullptr));
	}

	pending->done = true;
    }).detach ();
}

void DBusMediaSource::parseMetadata (DBusMessageIter& variant) {
    DBusMessageIter dict;
    dbus_message_iter_recurse (&variant, &dict);

    bool metadataUpdate = false;
    bool albumUpdate = false;

    while (dbus_message_iter_get_arg_type (&dict) == DBUS_TYPE_DICT_ENTRY) {
	DBusMessageIter entry;
	dbus_message_iter_recurse (&dict, &entry);

	const char* key = nullptr;
	dbus_message_iter_get_basic (&entry, &key);

	std::string keyStr = key ?: "";

	DBusMessageIter value;
	dbus_message_iter_next (&entry);
	dbus_message_iter_recurse (&entry, &value);

	if (keyStr == "xesam:title") {
	    const char* title = nullptr;
	    dbus_message_iter_get_basic (&value, &title);

	    std::string titleStr = title ?: "";

	    if (this->m_mediaInfo.title != titleStr) {
		this->m_mediaInfo.title = titleStr;
		metadataUpdate = true;
	    }
	} else if (keyStr == "xesam:artist") {
	    DBusMessageIter arr;

	    dbus_message_iter_recurse (&value, &arr);

	    if (dbus_message_iter_get_arg_type (&arr) == DBUS_TYPE_STRING) {
		const char* artist = nullptr;
		dbus_message_iter_get_basic (&arr, &artist);

		std::string artistStr = artist ?: "";

		if (this->m_mediaInfo.artist != artistStr) {
		    this->m_mediaInfo.artist = artistStr;
		    metadataUpdate = true;
		}
	    }
	} else if (keyStr == "xesam:albumArtist" || keyStr == "xesam:genre") {
	    DBusMessageIter arr;

	    dbus_message_iter_recurse (&value, &arr);

	    if (dbus_message_iter_get_arg_type (&arr) == DBUS_TYPE_STRING) {
		const char* text = nullptr;
		dbus_message_iter_get_basic (&arr, &text);

		std::string& target
		    = keyStr == "xesam:albumArtist" ? this->m_mediaInfo.albumArtist : this->m_mediaInfo.genres;

		if (target != (text ?: "")) {
		    target = text ?: "";
		    metadataUpdate = true;
		}
	    }
	} else if (keyStr == "xesam:album") {
	    const char* album = nullptr;
	    dbus_message_iter_get_basic (&value, &album);

	    std::string albumStr = album ?: "";

	    if (this->m_mediaInfo.album != albumStr) {
		this->m_mediaInfo.album = albumStr;
		albumUpdate = true;
	    }
	} else if (keyStr == "mpris:artUrl") {
	    const char* artUrl = nullptr;
	    dbus_message_iter_get_basic (&value, &artUrl);

	    std::string artUrlStr = artUrl ?: "";

	    if (artUrlStr.empty () && this->m_mediaInfo.url.has_value ()) {
		this->m_mediaInfo.url.reset ();
		albumUpdate = true;
	    } else if (this->m_mediaInfo.url.has_value () && artUrlStr != *this->m_mediaInfo.url) {
		this->m_mediaInfo.url = artUrlStr;
		albumUpdate = true;
	    } else if (!this->m_mediaInfo.url.has_value ()) {
		this->m_mediaInfo.url = artUrlStr;
		albumUpdate = true;
	    }
	} else if (keyStr == "mpris:length") {
	    int64_t length = 0;
	    dbus_message_iter_get_basic (&value, &length);

	    if (!WallpaperEngine::Data::Utils::sameFloat (this->m_mediaInfo.duration, length)) {
		this->m_mediaInfo.duration = length;
		metadataUpdate = true;
	    }
	}

	dbus_message_iter_next (&dict);
    }

    sLog.debug (
	"Player metadata received: title=", this->m_mediaInfo.title, ",artist=", this->m_mediaInfo.artist,
	",album=", this->m_mediaInfo.album, ",url=", this->m_mediaInfo.url.has_value () ? *this->m_mediaInfo.url : ""
    );

    if (metadataUpdate) {
	this->fireMetadataListeners ();
    }

    if (albumUpdate) {
	this->fireAlbumArtListeners ();
    }
}

void DBusMediaSource::parsePlaybackStatus (DBusMessageIter& variant, const char* sender) {
    const char* status = nullptr;
    dbus_message_iter_get_basic (&variant, &status);
    std::string statusStr = status ?: "";
    PlaybackState newState = PlaybackState::Stopped;

    if (statusStr == "Playing") {
	newState = PlaybackState::Playing;
    } else if (statusStr == "Paused") {
	newState = PlaybackState::Paused;
    }

    if (newState != this->m_mediaInfo.playbackState) {
	this->m_mediaInfo.playbackState = newState;
	this->fireMetadataListeners ();
    }
}

bool DBusMediaSource::isCurrentPlayer (const char* sender) const {
    return sender != nullptr && this->m_currentPlayer.has_value () && *this->m_currentPlayer == sender;
}

void DBusMediaSource::switchPlayer (const std::string& player) {
    this->m_currentPlayer = player;
    this->m_mediaInfo.playbackState = PlaybackState::Playing;
    this->initialStatusFetch ();
    this->fireMetadataListeners ();
}

std::optional<std::string> DBusMediaSource::uniqueName (const std::string& name) {
    if (!this->isConnected ()) {
	return std::nullopt;
    }

    const char* nameText = name.c_str ();
    DBusError err;
    dbus_error_init (&err);

    DBusMessage* msg = dbus_message_new_method_call (
	"org.freedesktop.DBus", "/org/freedesktop/DBus", "org.freedesktop.DBus", "GetNameOwner"
    );
    Data::Utils::ScopeGuard guard ([msg] { dbus_message_unref (msg); });
    dbus_message_append_args (msg, DBUS_TYPE_STRING, &nameText, DBUS_TYPE_INVALID);

    DBusMessage* reply = dbus_connection_send_with_reply_and_block (this->m_connection, msg, -1, &err);

    if (reply == nullptr) {
	dbus_error_free (&err);
	return std::nullopt;
    }

    Data::Utils::ScopeGuard replyGuard ([reply] { dbus_message_unref (reply); });
    const char* owner = nullptr;

    if (!dbus_message_get_args (reply, &err, DBUS_TYPE_STRING, &owner, DBUS_TYPE_INVALID) || owner == nullptr) {
	dbus_error_free (&err);
	return std::nullopt;
    }

    return std::string (owner);
}

void DBusMediaSource::parsePosition (DBusMessageIter& variant) {
    int64_t position = 0;
    dbus_message_iter_get_basic (&variant, &position);

    if (!WallpaperEngine::Data::Utils::sameFloat (this->m_mediaInfo.position, position)) {
	this->m_mediaInfo.position = position;
	this->fireMetadataListeners ();
    }
}

void DBusMediaSource::update () {
    this->tryReconnect ();

    if (this->m_missed) {
	this->m_missed = false;
	this->resync ();
    }

    this->MediaSource::update ();
    this->drain ();
}

void DBusMediaSource::drain () {
    if (this->m_connection != nullptr && !Desktop::drainBus (this->m_connection)) {
	this->handleLoss ();
    }
}

void DBusMediaSource::discard () {
    if (this->m_connection == nullptr) {
	return;
    }

    this->m_discarding = true;
    const bool alive = Desktop::drainBus (this->m_connection);
    this->m_discarding = false;

    if (!alive) {
	this->handleLoss ();
    }
}

// signals were thrown away while nothing was shown, the player is asked for what changed meanwhile
void DBusMediaSource::resync () {
    if (!this->m_currentPlayer.has_value ()) {
	this->m_nextUpdate = {};
	return;
    }

    this->initialStatusFetch ();

    DBusMessage* reply = this->dbusMessage (
	this->m_currentPlayer.value ().c_str (), "/org/mpris/MediaPlayer2", "org.freedesktop.DBus.Properties", "Get",
	"org.mpris.MediaPlayer2.Player", "PlaybackStatus"
    );

    if (reply == nullptr) {
	this->m_currentPlayer.reset ();
	this->m_nextUpdate = {};
	return;
    }

    Data::Utils::ScopeGuard guard ([reply] { dbus_message_unref (reply); });
    DBusMessageIter outer;
    dbus_message_iter_init (reply, &outer);

    DBusMessageIter variant;
    dbus_message_iter_recurse (&outer, &variant);

    if (dbus_message_iter_get_arg_type (&variant) != DBUS_TYPE_STRING) {
	return;
    }

    DBusMessageIter statusIter = variant;
    const char* status = nullptr;
    dbus_message_iter_get_basic (&statusIter, &status);

    this->parsePlaybackStatus (variant, this->m_currentPlayer->c_str ());

    // another player may have started while its signals were thrown away, detectPlayer only takes one that plays
    if (status == nullptr || std::string_view (status) != "Playing") {
	const auto previous = this->m_currentPlayer;
	this->detectPlayer ();

	if (this->m_currentPlayer != previous) {
	    this->initialStatusFetch ();
	}
    }
}

DBusMessage* DBusMediaSource::dbusMessage (
    const char* bus_name, const char* path, const char* interface, const char* method, const char* iface,
    const char* prop
) {
    if (!this->isConnected ()) {
	return nullptr;
    }

    DBusError err;
    dbus_error_init (&err);

    DBusMessage* msg = dbus_message_new_method_call (bus_name, path, interface, method);
    Data::Utils::ScopeGuard guard ([msg] { dbus_message_unref (msg); });

    if (iface != nullptr && prop != nullptr) {
	dbus_message_append_args (msg, DBUS_TYPE_STRING, &iface, DBUS_TYPE_STRING, &prop, DBUS_TYPE_INVALID);
    }

    DBusMessage* reply = dbus_connection_send_with_reply_and_block (m_connection, msg, -1, &err);

    if (reply == nullptr) {
	if (dbus_error_has_name (&err, DBUS_ERROR_SERVICE_UNKNOWN)
	    || dbus_error_has_name (&err, DBUS_ERROR_NAME_HAS_NO_OWNER)
	    || dbus_error_has_name (&err, DBUS_ERROR_UNKNOWN_OBJECT)) {
	    sLog.debug ("DBus player ", bus_name, " went away: ", err.message);
	} else {
	    sLog.error ("DBus error: ", err.message, " (", err.name, ")");
	}

	dbus_error_free (&err);
	return nullptr;
    }

    return reply;
}

void DBusMediaSource::detectPlayer () {
    DBusMessage* reply
	= this->dbusMessage ("org.freedesktop.DBus", "/org/freedesktop/DBus", "org.freedesktop.DBus", "ListNames");

    if (reply == nullptr) {
	return;
    }

    Data::Utils::ScopeGuard guard ([reply] { dbus_message_unref (reply); });
    std::vector<std::string> players;
    DBusMessageIter iter;
    dbus_message_iter_init (reply, &iter);

    if (dbus_message_iter_get_arg_type (&iter) != DBUS_TYPE_ARRAY) {
	return;
    }

    DBusMessageIter array;
    dbus_message_iter_recurse (&iter, &array);

    while (dbus_message_iter_get_arg_type (&array) != DBUS_TYPE_INVALID) {
	char* name;
	dbus_message_iter_get_basic (&array, &name);

	std::string service = name;

	if (service.starts_with ("org.mpris.MediaPlayer2.")) {
	    players.push_back (service);
	}

	dbus_message_iter_next (&array);
    }

    if (players.empty ()) {
	return;
    }

    for (const auto& player : players) {
	reply = this->dbusMessage (
	    player.c_str (), "/org/mpris/MediaPlayer2", "org.freedesktop.DBus.Properties", "Get",
	    "org.mpris.MediaPlayer2.Player", "PlaybackStatus"
	);

	if (reply == nullptr) {
	    continue;
	}

	Data::Utils::ScopeGuard guard2 ([reply] { dbus_message_unref (reply); });

	DBusMessageIter outer;
	dbus_message_iter_init (reply, &outer);
	DBusMessageIter variant;
	dbus_message_iter_recurse (&outer, &variant);

	const char* status = nullptr;

	if (dbus_message_iter_get_arg_type (&variant) != DBUS_TYPE_STRING) {
	    continue;
	}

	dbus_message_iter_get_basic (&variant, &status);

	if (status == nullptr || std::string_view (status) != "Playing") {
	    continue;
	}

	// signals carry the unique bus name, keep that one so they can be told apart
	if (const auto owner = this->uniqueName (player)) {
	    this->m_currentPlayer = *owner;
	    this->parsePlaybackStatus (variant, owner->c_str ());
	    break;
	}
    }
}

void DBusMediaSource::initialStatusFetch () {
    if (this->m_currentPlayer.has_value () == false) {
	return;
    }

    DBusMessage* reply = this->dbusMessage (
	this->m_currentPlayer.value ().c_str (), "/org/mpris/MediaPlayer2", "org.freedesktop.DBus.Properties", "Get",
	"org.mpris.MediaPlayer2.Player", "Metadata"
    );

    if (reply == nullptr) {
	return;
    }

    Data::Utils::ScopeGuard guard ([reply] { dbus_message_unref (reply); });
    DBusMessageIter outer;
    dbus_message_iter_init (reply, &outer);

    DBusMessageIter variant;
    dbus_message_iter_recurse (&outer, &variant);

    this->parseMetadata (variant);
}

void DBusMediaSource::performUpdate () {
    if (this->m_connection == nullptr) {
	return;
    }

    if (!this->isConnected ()) {
	this->handleLoss ();
	return;
    }

    if (!this->m_currentPlayer.has_value ()) {
	this->detectPlayer ();
	this->initialStatusFetch ();
	return;
    }

    DBusMessage* reply = this->dbusMessage (
	this->m_currentPlayer.value ().c_str (), "/org/mpris/MediaPlayer2", "org.freedesktop.DBus.Properties", "Get",
	"org.mpris.MediaPlayer2.Player", "Position"
    );

    if (reply == nullptr) {
	this->m_currentPlayer.reset ();
	return;
    }

    Data::Utils::ScopeGuard guard ([reply] { dbus_message_unref (reply); });
    DBusMessageIter outer;
    dbus_message_iter_init (reply, &outer);

    DBusMessageIter variant;
    dbus_message_iter_recurse (&outer, &variant);

    dbus_int64_t position = 0;
    dbus_message_iter_get_basic (&variant, &position);

    if (!WallpaperEngine::Data::Utils::sameFloat (this->m_mediaInfo.position, position)) {
	this->m_mediaInfo.position = position;
	this->fireMetadataListeners ();
    }
}