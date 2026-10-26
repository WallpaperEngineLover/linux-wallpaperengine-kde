// kscreen_mirror <clone> <source>|off - makes output <clone> a copy of output <source> (kscreen-doctor can't do that).
// Build in a Plasma 6 guest (libkscreen-devel): g++ -std=c++20 -fPIC kscreen_mirror.cpp -o kscreen_mirror $(pkg-config
// --cflags --libs Qt6Gui) -I/usr/include/KF6 -I/usr/include/KF6/KScreen -lKF6Screen Run it in the session with
// QT_QPA_PLATFORM=wayland (the KWayland backend refuses other platforms).
#include <KScreen/Config>
#include <KScreen/GetConfigOperation>
#include <KScreen/Output>
#include <KScreen/SetConfigOperation>
#include <QDebug>
#include <QGuiApplication>
#include <cstdio>

int main (int argc, char** argv) {
    QGuiApplication app (argc, argv);
    if (argc < 3) {
	qWarning () << "usage: kscreen_mirror <clone> <source>|off";
	return 2;
    }
    const QString cloneName = argv[1], sourceName = argv[2];
    auto get = new KScreen::GetConfigOperation ();
    get->exec ();
    if (get->hasError () || !get->config ()) {
	fprintf (stderr, "no config: %s\n", qPrintable (get->errorString ()));
	return 1;
    }
    auto config = get->config ();
    KScreen::OutputPtr clone, source;
    for (const auto& o : config->outputs ()) {
	if (o->name () == cloneName) {
	    clone = o;
	}
	if (o->name () == sourceName) {
	    source = o;
	}
    }
    if (!clone || (!source && sourceName != "off")) {
	fprintf (stderr, "output not found, have:");
	for (const auto& o : config->outputs ()) {
	    fprintf (stderr, " %s(%d)", qPrintable (o->name ()), o->id ());
	}
	fprintf (stderr, "\n");
	return 1;
    }
    clone->setReplicationSource (source ? source->id () : 0);
    auto set = new KScreen::SetConfigOperation (config);
    set->exec ();
    if (set->hasError ()) {
	fprintf (stderr, "set failed: %s\n", qPrintable (set->errorString ()));
    }
    qInfo () << "clone" << clone->id () << "source" << (source ? source->id () : 0) << "now"
	     << clone->replicationSource ();
    return 0;
}
