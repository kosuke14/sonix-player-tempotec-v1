#include "spotify.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "src/system/audio/audio.h"
#include "src/system/core/config.h"
#include "src/system/core/utils.h"
#include "src/system/device/sysinfo.h"
#include "src/system/streaming/spotifysink.h"

// The receiver's three interfaces, as q2-librespot names them. They are the
// fork's additions and its whole reason for existing on a player like this one:
// an unmodified librespot would be audible and unreachable.
#define SPOTIFY_STATUS_FILE "/tmp/sonix-spotify.state"
#define SPOTIFY_COVER_SUFFIX ".jpg" // beside the status file, what --status-file writes itself
#define SPOTIFY_CONTROL_SOCKET "/tmp/sonix-spotify.sock"
#define SPOTIFY_BROWSE_SOCKET "/tmp/sonix-spotify.browse.sock"
#define SPOTIFY_PCM_FIFO "/tmp/sonix-spotify.pcm"
#define SPOTIFY_LOG "/tmp/sonix-spotify.log"

// The card folder, and what has to be in it. `.spotify` is the same name the
// Q2 uses, so one card can serve both players and the documentation of either
// one applies.
#define SPOTIFY_CARD_DIR ".spotify"
#define SPOTIFY_BINARY "librespot"
#define SPOTIFY_CACHE_DIR "cache"

// The page's requests are allowed the protocol's 15 s, and the control thread
// wakes this often to look at the status file and at the child.
#define SPOTIFY_BROWSE_TIMEOUT_MS 15000
#define SPOTIFY_TICK_MS 200
#define SPOTIFY_STATUS_POLL_MS 500

// How long the receiver is given to go away after SIGTERM before it is killed.
// It has to flush its cache and its credentials, so not instant -- but it is
// leaving either way.
#define SPOTIFY_TERM_MS 3000

// A line of the status file, and the whole of it. The file is nine lines of at
// most a title's worth of text each.
#define SPOTIFY_STATUS_MAX 4096

// ---------------------------------------------------------------------------
// what this module keeps
// ---------------------------------------------------------------------------

static char card_dir[512];
static char binary_path[600];
static char cache_path[600];
static char status_path[sizeof(SPOTIFY_STATUS_FILE)] = SPOTIFY_STATUS_FILE;
static char cover_path[sizeof(SPOTIFY_STATUS_FILE) + sizeof(SPOTIFY_COVER_SUFFIX)];
static char control_path[sizeof(SPOTIFY_CONTROL_SOCKET)] = SPOTIFY_CONTROL_SOCKET;
static char browse_path[sizeof(SPOTIFY_BROWSE_SOCKET)] = SPOTIFY_BROWSE_SOCKET;
static char pcm_path[sizeof(SPOTIFY_PCM_FIFO)] = SPOTIFY_PCM_FIFO;
static char log_path[sizeof(SPOTIFY_LOG)] = SPOTIFY_LOG;

static pthread_mutex_t state_lock = PTHREAD_MUTEX_INITIALIZER;
static spotify_state_t state;
static unsigned state_serial;

// The player's own transport keys belong to Spotify while this is set: it was
// the last thing that played. Cleared when local playback takes the DAC back,
// because from that moment the keys belong to whatever is being listened to.
static bool owns_playback;

// Local playback took the DAC from under a playing receiver. Needed because the
// receiver's own status file takes a moment to say so: without it, the half
// second between the hand-over and the phone noticing would send the user's
// next key to a receiver that is no longer making sound. Cleared as soon as
// audio flows again, which is Spotify being listened to once more.
static bool handed_over;

static pthread_mutex_t ctl_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t ctl_cond = PTHREAD_COND_INITIALIZER;
static bool ctl_started;
static bool desired;
static bool actual;
static bool running;
static pid_t child_pid;
static char error_text[200];

// Never a whole page: the browser's worker owns the reply, this only says
// whether one is in flight, which is what keeps Wi-Fi up.
static int browse_busy;
static int request_counter; // the request numbers the protocol echoes back

static bool page_open;
static int64_t last_active_ms;
static bool ever_played;
static bool idle_stopped;

// ---------------------------------------------------------------------------

static int64_t now_ms(void) {
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void set_error(const char *fmt, ...) {
	va_list args;
	va_start(args, fmt);
	vsnprintf(error_text, sizeof(error_text), fmt, args);
	va_end(args);
}

// ---------------------------------------------------------------------------
// settings
// ---------------------------------------------------------------------------

int spotify_bitrate(void) {
	long value = config_get_int("spotify", "bitrate", 160);
	return (value == 96 || value == 320) ? (int)value : 160;
}

void spotify_set_bitrate(int kbps) {
	config_set_int("spotify", "bitrate", kbps == 96 || kbps == 320 ? kbps : 160);
	config_save();
}

const char *spotify_receiver_name(void) {
	static char name[96];
	const char *configured = config_get("spotify", "name", "");
	if (configured && configured[0]) {
		snprintf(name, sizeof(name), "%s", configured);
		return name;
	}

	const char *device = sysinfo_device_name();
	if (device && device[0]) {
		snprintf(name, sizeof(name), "%s", device);
		return name;
	}
	snprintf(name, sizeof(name), "Sonix Player");
	return name;
}

void spotify_set_receiver_name(const char *name) {
	config_set("spotify", "name", name ? name : "");
	config_save();
}

int spotify_cache_mb(void) {
	long value = config_get_int("spotify", "cache_mb", 1024);
	if (value < 0) {
		value = 0;
	}
	if (value > 32768) {
		value = 32768;
	}
	return (int)value;
}

void spotify_set_cache_mb(int mb) {
	if (mb < 0) {
		mb = 0;
	}
	config_set_int("spotify", "cache_mb", mb);
	config_save();
}

int spotify_idle_stop_secs(void) {
	long value = config_get_int("spotify", "idle_stop", 0);
	if (value < 0) {
		value = 0;
	}
	if (value > 86400) {
		value = 86400;
	}
	return (int)value;
}

void spotify_set_idle_stop_secs(int seconds) {
	if (seconds < 0) {
		seconds = 0;
	}
	config_set_int("spotify", "idle_stop", seconds);
	config_save();
}

static bool debug_logging(void) { return config_get_bool("spotify", "debug", false); }

// ---------------------------------------------------------------------------
// paths
// ---------------------------------------------------------------------------

void spotify_init(const char *sd_root) {
	if (!sd_root || !sd_root[0]) {
		card_dir[0] = '\0';
		binary_path[0] = '\0';
		cache_path[0] = '\0';
	} else {
		snprintf(card_dir, sizeof(card_dir), "%s/%s", sd_root, SPOTIFY_CARD_DIR);
		snprintf(binary_path, sizeof(binary_path), "%s/%s", card_dir, SPOTIFY_BINARY);
		snprintf(cache_path, sizeof(cache_path), "%s/%s", card_dir, SPOTIFY_CACHE_DIR);
	}
	snprintf(cover_path, sizeof(cover_path), "%s%s", status_path, SPOTIFY_COVER_SUFFIX);

	// The DAC hand-back. Registered here, and again on every start because it
	// costs nothing, alongside AirPlay's own -- audio_set_external_release_cb()
	// keeps a list and asks everyone, since the two receivers can each be
	// switched on and only one of them can be holding the PCM.
	audio_set_external_release_cb(spotifysink_release_output);
}

bool spotify_available(void) {
	return binary_path[0] != '\0' && access(binary_path, X_OK) == 0;
}

const char *spotify_dir(void) { return card_dir; }

void spotify_cover_path(char *out, size_t size) {
	if (!out || !size) {
		return;
	}
	snprintf(out, size, "%s", cover_path);
}

// ---------------------------------------------------------------------------
// the status file
// ---------------------------------------------------------------------------

static bool status_field(const char *file, const char *key, char *out, size_t size) {
	char needle[32];
	snprintf(needle, sizeof(needle), "\n%s=", key);

	const char *at = strstr(file, needle);
	if (!at) {
		out[0] = '\0';
		return false;
	}
	at += strlen(needle);

	size_t n = 0;
	while (at[n] && at[n] != '\n' && n + 1 < size) {
		out[n] = at[n];
		n++;
	}
	out[n] = '\0';
	return true;
}

static int status_field_int(const char *file, const char *key) {
	char value[24];
	if (!status_field(file, key, value, sizeof(value))) {
		return -1;
	}
	return (int)strtol(value, NULL, 10);
}

// Reads what the receiver wrote. Every field is optional: a receiver that has
// just started has written only `state=none`, and one that lost its session
// writes the same thing back.
static bool status_read(spotify_state_t *out) {
	int fd = open(status_path, O_RDONLY);
	if (fd < 0) {
		return false;
	}

	char buffer[SPOTIFY_STATUS_MAX];
	ssize_t n = read(fd, buffer, sizeof(buffer) - 1);
	close(fd);
	if (n <= 0) {
		return false;
	}
	buffer[n] = '\0';

	// The keys are matched with a leading newline so that `position=` cannot
	// answer for a key that ends in the same text, and the first line is given
	// one by hand.
	char file[SPOTIFY_STATUS_MAX + 2];
	file[0] = '\n';
	memcpy(file + 1, buffer, (size_t)n + 1);

	memset(out, 0, sizeof(*out));

	char value[SPOTIFY_TEXT_MAX];
	if (status_field(file, "state", value, sizeof(value))) {
		if (strcmp(value, "playing") == 0) {
			out->state = SPOTIFY_PLAY_PLAYING;
		} else if (strcmp(value, "paused") == 0) {
			out->state = SPOTIFY_PLAY_PAUSED;
		} else if (strcmp(value, "stopped") == 0) {
			out->state = SPOTIFY_PLAY_STOPPED;
		} else {
			out->state = SPOTIFY_PLAY_NONE;
		}
	} else {
		out->state = SPOTIFY_PLAY_NONE;
	}
	out->session = out->state != SPOTIFY_PLAY_NONE;

	status_field(file, "track", out->track, sizeof(out->track));
	status_field(file, "title", out->title, sizeof(out->title));
	status_field(file, "artist", out->artist, sizeof(out->artist));
	status_field(file, "album", out->album, sizeof(out->album));

	int duration = status_field_int(file, "duration");
	int position = status_field_int(file, "position");
	int at = status_field_int(file, "at");
	if (duration > 0) {
		out->duration_ms = duration;
	}
	if (position >= 0) {
		out->position_ms = position;
	}
	if (at >= 0 && out->state == SPOTIFY_PLAY_PLAYING) {
		// The file says where the track was at one moment; the clock says how
		// long ago that was. This is the same arithmetic Q2 Pod does with the
		// same two fields, and it is why `at` is a CLOCK_MONOTONIC value --
		// the reader and the writer are on the same machine.
		int elapsed = (int)(now_ms() - at);
		if (elapsed > 0) {
			out->position_ms += elapsed;
		}
	}
	if (out->duration_ms > 0 && out->position_ms > out->duration_ms) {
		out->position_ms = out->duration_ms;
	}

	out->cover = status_field_int(file, "cover") == 1;
	return true;
}

// Publishes a new snapshot, bumping the serial only when something a page
// draws has actually moved.
static void state_publish(const spotify_state_t *next, bool audio) {
	pthread_mutex_lock(&state_lock);

	spotify_state_t merged = *next;
	merged.playing = audio;

	// The position moves with the clock, so comparing it would bump the serial
	// every poll. What a page watches for is a move between fields -- a face
	// turning over is the page's own timer's business.
	spotify_state_t compare = merged;
	compare.position_ms = state.position_ms;

	if (compare.state != state.state || compare.session != state.session || compare.playing != state.playing ||
		compare.duration_ms != state.duration_ms || strcmp(compare.track, state.track) != 0 ||
		strcmp(compare.title, state.title) != 0 || strcmp(compare.artist, state.artist) != 0 ||
		strcmp(compare.album, state.album) != 0 || compare.cover != state.cover) {
		state_serial++;
	}

	// The latch. Audio flowing is the strongest statement there is: it is what
	// the user is listening to, whoever paused what.
	if (audio) {
		handed_over = false;
		owns_playback = true;
		ever_played = true;
	} else if (merged.state == SPOTIFY_PLAY_PLAYING && !handed_over) {
		owns_playback = true;
		ever_played = true;
	} else if (merged.state == SPOTIFY_PLAY_NONE) {
		owns_playback = false;
		handed_over = false;
	}

	state = merged;
	pthread_mutex_unlock(&state_lock);
}

static void state_clear(void) {
	pthread_mutex_lock(&state_lock);
	memset(&state, 0, sizeof(state));
	state_serial++;
	pthread_mutex_unlock(&state_lock);
}

void spotify_get_state(spotify_state_t *out) {
	if (!out) {
		return;
	}
	pthread_mutex_lock(&state_lock);
	*out = state;
	pthread_mutex_unlock(&state_lock);
}

unsigned spotify_serial(void) {
	pthread_mutex_lock(&state_lock);
	unsigned serial = state_serial;
	pthread_mutex_unlock(&state_lock);
	return serial;
}

int spotify_silent_ms(void) { return spotifysink_silent_ms(); }

// ---------------------------------------------------------------------------
// the control socket
// ---------------------------------------------------------------------------

static void control_send(const char *bytes, size_t length) {
	int fd = socket(AF_UNIX, SOCK_DGRAM, 0);
	if (fd < 0) {
		return;
	}

	struct sockaddr_un addr;
	memset(&addr, 0, sizeof(addr));
	addr.sun_family = AF_UNIX;
	snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", control_path);

	// Best effort by design: a receiver that is not up, or that has just gone
	// away, means the datagram goes nowhere and the key falls through to the
	// local transport, which is what a player with no Spotify running should
	// do with a play key.
	sendto(fd, bytes, length, MSG_DONTWAIT, (struct sockaddr *)&addr, sizeof(addr));
	close(fd);
}

void spotify_control(spotify_command_t command) {
	switch (command) {
	case SPOTIFY_CMD_PLAY_PAUSE:
		control_send("p", 1);
		break;
	case SPOTIFY_CMD_PLAY:
		control_send("P", 1);
		break;
	case SPOTIFY_CMD_PAUSE:
		control_send("s", 1);
		break;
	case SPOTIFY_CMD_NEXT:
		control_send("n", 1);
		break;
	case SPOTIFY_CMD_PREV:
		control_send("b", 1);
		break;
	}
	spotify_activity();
}

void spotify_seek_ms(int ms) {
	if (ms < 0) {
		return;
	}
	char buffer[24];
	int n = snprintf(buffer, sizeof(buffer), "S%d", ms);
	if (n > 0 && (size_t)n < sizeof(buffer)) {
		control_send(buffer, (size_t)n);
	}
	spotify_activity();
}

// ---------------------------------------------------------------------------
// ownership of the keys
// ---------------------------------------------------------------------------

bool spotify_owns_playback(void) {
	pthread_mutex_lock(&state_lock);
	bool owns = running && owns_playback;
	pthread_mutex_unlock(&state_lock);
	if (!owns) {
		return false;
	}

	// ...unless a local track has the output. A receiver that is paused holds
	// nothing, so starting a track from the card never asks it to let go, and
	// without this the side keys would keep going to a phone that is not
	// playing anything while a track on the card is.
	return audio_get_status() != AUDIO_STATUS_PLAYING;
}

void spotify_forget_active(void) {
	pthread_mutex_lock(&state_lock);
	owns_playback = false;
	handed_over = true;
	pthread_mutex_unlock(&state_lock);
}

// ---------------------------------------------------------------------------
// the browser
// ---------------------------------------------------------------------------

bool spotify_browse(const char *op, const char *const *args, int nargs, spotify_reply_t *out) {
	if (!out) {
		return false;
	}

	if (!spotify_running()) {
		memset(out, 0, sizeof(*out));
		out->status = SPOTIFY_REPLY_NOSOCKET;
		snprintf(out->message, sizeof(out->message), "the receiver is not running");
		return false;
	}

	__atomic_add_fetch(&browse_busy, 1, __ATOMIC_SEQ_CST);
	unsigned id = (unsigned)__atomic_add_fetch(&request_counter, 1, __ATOMIC_SEQ_CST);
	bool ok = spotify_browse_call(browse_path, id, op, args, nargs, out, SPOTIFY_BROWSE_TIMEOUT_MS);
	__atomic_sub_fetch(&browse_busy, 1, __ATOMIC_SEQ_CST);

	spotify_activity();
	return ok;
}

bool spotify_browse_busy(void) { return __atomic_load_n(&browse_busy, __ATOMIC_SEQ_CST) > 0; }

bool spotify_backend_version(char *out, size_t size) {
	if (out && size) {
		out[0] = '\0';
	}
	if (!spotify_running()) {
		return false;
	}

	spotify_reply_t reply;
	if (!spotify_browse("caps", NULL, 0, &reply)) {
		return false;
	}
	if (out && size) {
		snprintf(out, size, "%s", reply.message);
	}
	return true;
}

void spotify_log_tail(char *out, size_t size) {
	if (!out || !size) {
		return;
	}
	out[0] = '\0';

	int fd = open(log_path, O_RDONLY);
	if (fd < 0) {
		return;
	}
	off_t end = lseek(fd, 0, SEEK_END);
	const off_t want = 512;
	off_t from = end > want ? end - want : 0;
	if (lseek(fd, from, SEEK_SET) < 0) {
		close(fd);
		return;
	}

	char buffer[600];
	ssize_t n = read(fd, buffer, sizeof(buffer) - 1);
	close(fd);
	if (n <= 0) {
		return;
	}
	buffer[n] = '\0';

	// The tail usually starts in the middle of a line; the log's own timestamps
	// make a half line look wrong, so start from the first complete one.
	char *text = buffer;
	if (from > 0) {
		char *newline = strchr(buffer, '\n');
		if (newline) {
			text = newline + 1;
		}
	}

	size_t length = strlen(text);
	// Trim the trailing newline the last row adds, and take the last lines that
	// fit: the newest thing the receiver said is what explains it.
	while (length > 0 && (text[length - 1] == '\n' || text[length - 1] == '\r')) {
		text[--length] = '\0';
	}
	if (length >= size) {
		text += length - (size - 1);
	}
	snprintf(out, size, "%s", text);
}

// ---------------------------------------------------------------------------
// starting and stopping the receiver
// ---------------------------------------------------------------------------

// What the sink calls when local playback takes the DAC: the receiver is told
// to stop playing into it, so the phone's session pauses instead of going
// silent with the device in someone else's hands. The keys go back to local
// playback at the same moment -- it is playing now, and the side buttons
// control what is playing.
static void receiver_pause(void) {
	spotify_control(SPOTIFY_CMD_PAUSE);
	spotify_forget_active();
}

static void runtime_cleanup(void) {
	unlink(status_path);
	unlink(cover_path);
	unlink(control_path);
	unlink(browse_path);
}

static bool child_spawn(void) {
	char argv_storage[16][128];
	char *argv[48];
	int n = 0;

	char bitrate[16];
	snprintf(bitrate, sizeof(bitrate), "%d", spotify_bitrate());
	char cache_limit[32];
	snprintf(cache_limit, sizeof(cache_limit), "%dM", spotify_cache_mb());

	// The card folder has to exist for the credential cache inside it: the
	// saved login is what makes the second start not ask the phone to pair
	// again, and librespot will not create a --cache directory for itself.
	mkdir(card_dir, 0755);
	mkdir(cache_path, 0755);

	argv[n++] = binary_path;
	argv[n++] = (char *)"--name";
	argv[n++] = (char *)spotify_receiver_name();
	argv[n++] = (char *)"--backend";
	argv[n++] = (char *)"pipe";
	argv[n++] = (char *)"--device";
	argv[n++] = pcm_path;
	argv[n++] = (char *)"--format";
	argv[n++] = (char *)"S16";
	argv[n++] = (char *)"--bitrate";
	argv[n++] = bitrate;
	// The volume is the player's DAC's, as it is for AirPlay and for every
	// other source: librespot must not apply a second, invisible one on top.
	argv[n++] = (char *)"--volume-ctrl";
	argv[n++] = (char *)"fixed";
	argv[n++] = (char *)"--initial-volume";
	argv[n++] = (char *)"100";
	argv[n++] = (char *)"--cache";
	argv[n++] = cache_path;
	if (spotify_cache_mb() > 0) {
		// Complete cached tracks are what the browser's Downloaded list replays
		// without a network, so this is the one setting that decides whether
		// that list can do anything.
		argv[n++] = (char *)"--cache-size-limit";
		argv[n++] = cache_limit;
	} else {
		argv[n++] = (char *)"--disable-audio-cache";
	}
	argv[n++] = (char *)"--status-file";
	argv[n++] = status_path;
	argv[n++] = (char *)"--control-socket";
	argv[n++] = control_path;
	argv[n++] = (char *)"--browse-socket";
	argv[n++] = browse_path;

	if (debug_logging()) {
		// librespot's own verbosity flags, one per level.
		argv[n++] = (char *)"-vv";
	}

	// Whatever the card's owner wants on top: a proxy, normalisation, a
	// different discovery mode. Split on whitespace, no quoting -- the config
	// file is a plain INI and a real parser for this would be more machinery
	// than the feature is worth.
	const char *extra = config_get("spotify", "extra", "");
	int extra_used = 0;
	if (extra && extra[0] && n < (int)(sizeof(argv) / sizeof(argv[0])) - 1) {
		const char *at = extra;
		while (*at && extra_used < 8 && n < (int)(sizeof(argv) / sizeof(argv[0])) - 1) {
			while (*at == ' ' || *at == '\t') {
				at++;
			}
			if (!*at) {
				break;
			}
			char *slot = argv_storage[extra_used];
			size_t length = 0;
			while (at[length] && at[length] != ' ' && at[length] != '\t' && length + 1 < sizeof(argv_storage[0])) {
				slot[length] = at[length];
				length++;
			}
			slot[length] = '\0';
			argv[n++] = slot;
			extra_used++;
			at += length;
		}
	}

	argv[n] = NULL;

	// The FIFO's read end is already open (spotifysink_start opens it before
	// the receiver is started, so that librespot's own blocking open of the
	// write end finds its reader waiting).
	pid_t pid = fork();
	if (pid < 0) {
		set_error("could not start the receiver: %s", strerror(errno));
		return false;
	}
	if (pid == 0) {
		// A session of its own, so a signal aimed at the player's process group
		// does not take the receiver out through the back door -- it is stopped
		// deliberately, and its cache is flushed on the way.
		setsid();

		// And if the player dies anyway, the kernel tells the receiver to go.
		// A librespot left holding the card while the storage layer unmounts is
		// a filesystem that is dirty on the next boot.
		prctl(PR_SET_PDEATHSIG, SIGTERM);

		int log_fd = open(log_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
		if (log_fd >= 0) {
			dup2(log_fd, STDOUT_FILENO);
			dup2(log_fd, STDERR_FILENO);
			if (log_fd > STDERR_FILENO) {
				close(log_fd);
			}
		} else {
			int null_fd = open("/dev/null", O_WRONLY);
			if (null_fd >= 0) {
				dup2(null_fd, STDOUT_FILENO);
				dup2(null_fd, STDERR_FILENO);
				if (null_fd > STDERR_FILENO) {
					close(null_fd);
				}
			}
		}

		// The card's own folder when it is there, and the root otherwise: a
		// receiver that writes anything relative must land on the card or
		// nowhere, never in the firmware's own directories, which is where the
		// player stands. A receiver with nowhere to stand is not run at all --
		// and the message goes into the log the parent reads back, since the
		// receiver is now writing to it.
		if (chdir(card_dir) != 0 && chdir("/") != 0) {
			fprintf(stderr, "spotify: no working directory: %s\n", strerror(errno));
			_exit(126);
		}

		execv(binary_path, argv);
		fprintf(stderr, "spotify: execv(%s): %s\n", binary_path, strerror(errno));
		_exit(127);
	}

	child_pid = pid;
	printf("spotify: receiver started, pid %d\n", (int)pid);
	return true;
}

static void child_stop(void) {
	if (child_pid <= 0) {
		return;
	}

	kill(child_pid, SIGTERM);

	int64_t until = now_ms() + SPOTIFY_TERM_MS;
	for (;;) {
		int status = 0;
		pid_t reaped = waitpid(child_pid, &status, WNOHANG);
		if (reaped == child_pid || (reaped < 0 && errno == ECHILD)) {
			break;
		}
		if (now_ms() >= until) {
			printf("spotify: the receiver did not stop; killing it\n");
			kill(child_pid, SIGKILL);
			waitpid(child_pid, &status, 0);
			break;
		}
		usleep(50 * 1000);
	}

	child_pid = 0;
}

// The whole life of one enabled period, on the control thread: the receiver up,
// its status watched, and everything torn down on the way out.
static void service_run(void) {
	error_text[0] = '\0';
	ever_played = false;
	idle_stopped = false;
	last_active_ms = now_ms();
	state_clear();
	runtime_cleanup();

	// The FIFO first, then the reader, then the receiver: librespot opens the
	// write end when it starts playing, and a FIFO with no reader blocks that
	// open forever -- a receiver that is running and silent.
	bool sink = spotifysink_start(pcm_path, receiver_pause);

	if (!sink) {
		set_error("could not open the audio FIFO");
		desired = false;
		return;
	}

	if (!child_spawn()) {
		spotifysink_stop();
		desired = false;
		return;
	}

	pthread_mutex_lock(&ctl_lock);
	running = true;
	pthread_mutex_unlock(&ctl_lock);

	int64_t next_poll = now_ms();
	bool failed = false;

	for (;;) {
		pthread_mutex_lock(&ctl_lock);
		bool stopping = !desired;
		pthread_mutex_unlock(&ctl_lock);
		if (stopping) {
			break;
		}

		// The child going away on its own is a failure: a build that does not
		// know the fork's flags exits in its first second, and the page has to
		// be able to say so rather than show a switch that does nothing.
		int status = 0;
		pid_t reaped = waitpid(child_pid, &status, WNOHANG);
		if (reaped == child_pid) {
			char tail[200];
			spotify_log_tail(tail, sizeof(tail));
			if (WIFEXITED(status)) {
				set_error("the receiver stopped (%d)%s%s", WEXITSTATUS(status), tail[0] ? ": " : "", tail);
			} else {
				set_error("the receiver stopped%s%s", tail[0] ? ": " : "", tail);
			}
			child_pid = 0;
			failed = true;
			break;
		}

		int64_t now = now_ms();
		if (now >= next_poll) {
			next_poll = now + SPOTIFY_STATUS_POLL_MS;

			bool audio = spotifysink_playing();

			spotify_state_t snapshot;
			if (status_read(&snapshot)) {
				if (snapshot.state == SPOTIFY_PLAY_PLAYING || audio) {
					ever_played = true;
				}
				state_publish(&snapshot, audio);
			} else {
				spotify_state_t empty;
				memset(&empty, 0, sizeof(empty));
				state_publish(&empty, audio);
			}

			// "In use" is audio actually flowing, a request in flight, or
			// someone looking at the browser -- not the receiver's own claim
			// that it is playing. A receiver that says playing with nothing
			// coming out is exactly the case the idle stop exists for.
			if (audio || spotify_browse_busy() || page_open) {
				last_active_ms = now;
			}

			int idle = spotify_idle_stop_secs();
			if (idle > 0 && ever_played && !page_open && !spotify_browse_busy() && !spotifysink_playing() &&
				now - last_active_ms > (int64_t)idle * 1000) {
				printf("spotify: idle for %d s after playing; stopping the receiver\n", idle);
				// The switch goes off by itself here, so the page has to be able to
				// say why: the user will come back to a receiver that is off and
				// an unexplained toggle is a bug report.
				set_error("stopped after sitting idle for %d s", idle);
				idle_stopped = true;
				pthread_mutex_lock(&ctl_lock);
				desired = false;
				pthread_mutex_unlock(&ctl_lock);
				break;
			}
		}

		// A short sleep that a switch-off interrupts: the condvar is the same
		// one spotify_set_enabled() signals.
		pthread_mutex_lock(&ctl_lock);
		if (desired) {
			struct timespec until;
			clock_gettime(CLOCK_MONOTONIC, &until);
			until.tv_nsec += SPOTIFY_TICK_MS * 1000000L;
			if (until.tv_nsec >= 1000000000L) {
				until.tv_nsec -= 1000000000L;
				until.tv_sec++;
			}
			pthread_cond_timedwait(&ctl_cond, &ctl_lock, &until);
		}
		pthread_mutex_unlock(&ctl_lock);
	}

	// The sink goes first: it is still draining the FIFO, and a receiver that
	// keeps writing into a FIFO nobody reads blocks in write() rather than
	// noticing SIGTERM.
	spotifysink_stop();
	child_stop();
	runtime_cleanup();
	state_clear();

	pthread_mutex_lock(&state_lock);
	owns_playback = false;
	pthread_mutex_unlock(&state_lock);

	// `running` stays true until the teardown is done, so that spotify_shutdown()
	// waits for a receiver that really has gone rather than one that is on its
	// way out with the card still open.
	pthread_mutex_lock(&ctl_lock);
	running = false;
	if (failed || idle_stopped) {
		desired = false;
	}
	pthread_cond_broadcast(&ctl_cond);
	pthread_mutex_unlock(&ctl_lock);
}

static void *ctl_worker(void *unused) {
	(void)unused;

	for (;;) {
		pthread_mutex_lock(&ctl_lock);
		while (desired == actual) {
			pthread_cond_wait(&ctl_cond, &ctl_lock);
		}
		bool want = desired;
		pthread_mutex_unlock(&ctl_lock);

		if (want) {
			service_run();
		} else {
			// The receiver is stopped by service_run() on its way out; an
			// enabled receiver can never reach here with something running.
			pthread_mutex_lock(&ctl_lock);
			running = false;
			pthread_mutex_unlock(&ctl_lock);
		}

		pthread_mutex_lock(&ctl_lock);
		actual = want;
		pthread_cond_broadcast(&ctl_cond);
		pthread_mutex_unlock(&ctl_lock);
	}
	return NULL;
}

void spotify_set_enabled(bool on) {
	audio_set_external_release_cb(spotifysink_release_output);

	pthread_mutex_lock(&ctl_lock);

	if (!ctl_started) {
		pthread_t thread;
		if (pthread_create(&thread, NULL, ctl_worker, NULL) != 0) {
			pthread_mutex_unlock(&ctl_lock);
			set_error("no control thread");
			fprintf(stderr, "spotify: could not start the control thread\n");
			return;
		}
		pthread_detach(thread);
		ctl_started = true;
	}

	if (on && !spotify_available()) {
		desired = false;
		pthread_mutex_unlock(&ctl_lock);
		set_error("the card does not carry %s", binary_path[0] ? binary_path : SPOTIFY_BINARY);
		return;
	}

	desired = on;
	last_active_ms = now_ms();
	pthread_cond_broadcast(&ctl_cond);
	pthread_mutex_unlock(&ctl_lock);
}

bool spotify_enabled(void) {
	pthread_mutex_lock(&ctl_lock);
	bool on = desired;
	pthread_mutex_unlock(&ctl_lock);
	return on;
}

bool spotify_running(void) {
	pthread_mutex_lock(&ctl_lock);
	bool up = running;
	pthread_mutex_unlock(&ctl_lock);
	return up;
}

const char *spotify_status_text(void) {
	static char text[224];

	pthread_mutex_lock(&ctl_lock);
	bool on = desired;
	bool up = running;
	char error[sizeof(error_text)];
	snprintf(error, sizeof(error), "%s", error_text);
	pthread_mutex_unlock(&ctl_lock);

	if (!spotify_available()) {
		snprintf(text, sizeof(text), "the receiver is not on the card");
	} else if (error[0] && !up) {
		snprintf(text, sizeof(text), "%s", error);
	} else if (up) {
		snprintf(text, sizeof(text), "running");
	} else if (on) {
		snprintf(text, sizeof(text), "starting");
	} else {
		text[0] = '\0';
	}
	return text;
}

void spotify_shutdown(void) {
	if (!spotify_enabled() && !spotify_running()) {
		return;
	}

	spotify_set_enabled(false);

	// Waiting is the point: the storage layer unmounts the card a moment after
	// this returns, and a receiver still flushing its cache into a card that
	// has just been unmounted is a filesystem that is dirty on the next boot.
	int64_t until = now_ms() + SPOTIFY_TERM_MS + 2000;
	while (spotify_running() && now_ms() < until) {
		usleep(50 * 1000);
	}
}

// ---------------------------------------------------------------------------
// what the page tells the service
// ---------------------------------------------------------------------------

void spotify_page_open(bool open) {
	pthread_mutex_lock(&ctl_lock);
	page_open = open;
	last_active_ms = now_ms();
	pthread_mutex_unlock(&ctl_lock);
}

void spotify_activity(void) {
	pthread_mutex_lock(&ctl_lock);
	last_active_ms = now_ms();
	pthread_mutex_unlock(&ctl_lock);
}
