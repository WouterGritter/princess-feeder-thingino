// feederd: plays the Tuya Wi-Fi module for the Princess feeder's MCU and
// exposes it over MQTT. Single-threaded: one poll() loop over the MCU UART
// and the MQTT socket.
//
// MCU link: Tuya MCU serial protocol on /dev/ttyS0, 9600 8N1.
//   frame = 55 AA ver cmd lenH lenL data.. chk   (chk = byte sum mod 256)
//   we send ver 0x13, the MCU sends ver 0x10.
//   00 heartbeat (MCU asks every 1 s, we answer)    1c time sync (MCU asks)
//   03 network status = LED pattern                  06 set DP / 07 DP report
//   08 query all DPs
// DPs: 203 feed N (set), 215 dispensed N (report), 214 fault bitmap (bit 1 =
// food low), 240 aux short press (plus ~40 ms self-test blips), 241 aux held.

#include <mosquitto.h>

#include <cerrno>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <deque>
#include <map>
#include <string>
#include <vector>

#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <syslog.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

namespace {

// ---------------------------------------------------------------- logging

bool g_verbose = false;
bool g_stderr = false;

void logmsg(int prio, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
void logmsg(int prio, const char *fmt, ...) {
	if (prio == LOG_DEBUG && !g_verbose)
		return;
	char buf[512];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(buf, sizeof buf, fmt, ap);
	va_end(ap);
	syslog(prio, "%s", buf);
	if (g_stderr)
		fprintf(stderr, "%s\n", buf);
}
#define LOGD(...) logmsg(LOG_DEBUG, __VA_ARGS__)
#define LOGI(...) logmsg(LOG_INFO, __VA_ARGS__)
#define LOGW(...) logmsg(LOG_WARNING, __VA_ARGS__)
#define LOGE(...) logmsg(LOG_ERR, __VA_ARGS__)

int64_t now_ms() {
	timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return int64_t(ts.tv_sec) * 1000 + ts.tv_nsec / 1000000;
}

std::string hex(const uint8_t *p, size_t n) {
	std::string s;
	char b[4];
	for (size_t i = 0; i < n; i++) {
		snprintf(b, sizeof b, i ? " %02x" : "%02x", p[i]);
		s += b;
	}
	return s;
}

bool parse_int(const std::string &s, long &out) {
	if (s.empty())
		return false;
	char *end = nullptr;
	errno = 0;
	long v = strtol(s.c_str(), &end, 10);
	if (errno || *end != '\0')
		return false;
	out = v;
	return true;
}

std::string trim(const std::string &s) {
	size_t a = s.find_first_not_of(" \t\r\n");
	if (a == std::string::npos)
		return "";
	size_t b = s.find_last_not_of(" \t\r\n");
	return s.substr(a, b - a + 1);
}

// ---------------------------------------------------------------- config

struct Config {
	std::string mqtt_host; // required
	int mqtt_port = 1883;
	std::string mqtt_user;
	std::string mqtt_pass;
	std::string mqtt_client_id = "pet-feeder";
	std::string root_topic = "home/pet-feeder";
	int max_portions = 20;
	std::string led_idle = "off";
	int day_reset_hour = 4;
	std::string serial = "/dev/ttyS0";
	std::string state_file = "/etc/feederd.state";
};

bool led_mode_valid(const std::string &m);

bool load_config(const char *path, Config &c) {
	FILE *f = fopen(path, "r");
	if (!f) {
		LOGE("config %s: %s", path, strerror(errno));
		return false;
	}
	char line[512];
	int n = 0;
	bool ok = true;
	while (fgets(line, sizeof line, f)) {
		n++;
		std::string l = trim(line);
		if (l.empty() || l[0] == '#')
			continue;
		size_t eq = l.find('=');
		if (eq == std::string::npos) {
			LOGE("config %s:%d: no '=' in line", path, n);
			ok = false;
			continue;
		}
		std::string k = trim(l.substr(0, eq)), v = trim(l.substr(eq + 1));
		long iv = 0;
		if (k == "mqtt_host") c.mqtt_host = v;
		else if (k == "mqtt_port" && parse_int(v, iv)) c.mqtt_port = int(iv);
		else if (k == "mqtt_user") c.mqtt_user = v;
		else if (k == "mqtt_pass") c.mqtt_pass = v;
		else if (k == "mqtt_client_id") c.mqtt_client_id = v;
		else if (k == "root_topic") c.root_topic = v;
		else if (k == "max_portions" && parse_int(v, iv)) c.max_portions = int(iv);
		else if (k == "led_idle") c.led_idle = v;
		else if (k == "day_reset_hour" && parse_int(v, iv)) c.day_reset_hour = int(iv);
		else if (k == "serial") c.serial = v;
		else if (k == "state_file") c.state_file = v;
		else {
			LOGE("config %s:%d: unknown key or bad value: %s", path, n, l.c_str());
			ok = false;
		}
	}
	fclose(f);
	while (!c.root_topic.empty() && c.root_topic.back() == '/')
		c.root_topic.pop_back();
	if (c.mqtt_host.empty()) {
		LOGE("config: mqtt_host is required");
		ok = false;
	}
	if (c.root_topic.empty()) {
		LOGE("config: root_topic must not be empty");
		ok = false;
	}
	if (c.max_portions < 1 || c.max_portions > 50) {
		LOGE("config: max_portions must be 1..50");
		ok = false;
	}
	if (c.day_reset_hour < 0 || c.day_reset_hour > 23) {
		LOGE("config: day_reset_hour must be 0..23");
		ok = false;
	}
	if (c.led_idle == "idle" || !led_mode_valid(c.led_idle)) {
		LOGE("config: led_idle must be off, on, slow_blink or fast_blink");
		ok = false;
	}
	return ok;
}

// ---------------------------------------------------------------- LED

struct LedMode {
	const char *name;
	uint8_t status; // Tuya network-status byte the MCU turns into a pattern
};
// 3 goes dark and stays dark on this MCU, 7 (out of range) is a clean off.
const LedMode kLedModes[] = {
	{"off", 7}, {"on", 4}, {"slow_blink", 0}, {"fast_blink", 2},
};

bool led_mode_valid(const std::string &m) {
	if (m == "idle")
		return true;
	for (auto &l : kLedModes)
		if (m == l.name)
			return true;
	return false;
}

uint8_t led_status_for(const std::string &m) {
	for (auto &l : kLedModes)
		if (m == l.name)
			return l.status;
	return 7;
}

// ---------------------------------------------------------------- MCU

enum : uint8_t {
	CMD_HEARTBEAT = 0x00,
	CMD_NET_STATUS = 0x03,
	CMD_SET_DP = 0x06,
	CMD_DP_REPORT = 0x07,
	CMD_QUERY = 0x08,
	CMD_TIME = 0x1c,
};
enum : uint8_t {
	DP_FEED = 203,
	DP_FAULT = 214,
	DP_DISPENSED = 215,
	DP_AUX_SHORT = 240,
	DP_AUX_HELD = 241,
};
const uint8_t kHostVer = 0x13;
const uint8_t kFaultFoodLow = 0x02;

class Mcu {
public:
	bool open_port(const std::string &path) {
		fd_ = open(path.c_str(), O_RDWR | O_NOCTTY | O_NONBLOCK);
		if (fd_ < 0) {
			LOGE("open %s: %s", path.c_str(), strerror(errno));
			return false;
		}
		termios t{};
		if (tcgetattr(fd_, &t) < 0) {
			LOGE("tcgetattr %s: %s", path.c_str(), strerror(errno));
			return false;
		}
		cfmakeraw(&t);
		cfsetispeed(&t, B9600);
		cfsetospeed(&t, B9600);
		t.c_cflag |= CLOCAL | CREAD;
		t.c_cflag &= ~(CSTOPB | PARENB | CRTSCTS);
		if (tcsetattr(fd_, TCSANOW, &t) < 0) {
			LOGE("tcsetattr %s: %s", path.c_str(), strerror(errno));
			return false;
		}
		tcflush(fd_, TCIOFLUSH);
		return true;
	}
	int fd() const { return fd_; }

	bool send(uint8_t cmd, const uint8_t *data, size_t len) {
		std::vector<uint8_t> f = {0x55, 0xaa, kHostVer, cmd, uint8_t(len >> 8), uint8_t(len)};
		f.insert(f.end(), data, data + len);
		uint8_t sum = 0;
		for (uint8_t b : f)
			sum += b;
		f.push_back(sum);
		if (cmd != CMD_HEARTBEAT)
			LOGD("T31> %s", hex(f.data(), f.size()).c_str());
		size_t off = 0;
		while (off < f.size()) {
			ssize_t w = write(fd_, f.data() + off, f.size() - off);
			if (w < 0) {
				if (errno == EAGAIN || errno == EINTR) {
					usleep(2000);
					continue;
				}
				LOGE("uart write: %s", strerror(errno));
				return false;
			}
			off += size_t(w);
		}
		return true;
	}
	bool send(uint8_t cmd) { return send(cmd, nullptr, 0); }

	bool set_dp_value(uint8_t dp, uint32_t v) {
		uint8_t d[] = {dp, 0x02, 0x00, 0x04, uint8_t(v >> 24), uint8_t(v >> 16), uint8_t(v >> 8), uint8_t(v)};
		return send(CMD_SET_DP, d, sizeof d);
	}

	// Reads what's available; returns complete frames (cmd, payload).
	struct Frame {
		uint8_t cmd;
		std::vector<uint8_t> data;
	};
	bool read_frames(std::vector<Frame> &out) {
		uint8_t buf[256];
		for (;;) {
			ssize_t r = read(fd_, buf, sizeof buf);
			if (r > 0) {
				rx_.insert(rx_.end(), buf, buf + r);
				continue;
			}
			if (r < 0 && (errno == EAGAIN || errno == EINTR))
				break;
			if (r < 0) {
				LOGE("uart read: %s", strerror(errno));
				return false;
			}
			break;
		}
		while (rx_.size() >= 7) {
			if (rx_[0] != 0x55 || rx_[1] != 0xaa) {
				size_t skip = 1;
				while (skip < rx_.size() && rx_[skip] != 0x55)
					skip++;
				LOGW("uart: dropped %zu stray byte(s): %s", skip, hex(rx_.data(), skip).c_str());
				rx_.erase(rx_.begin(), rx_.begin() + skip);
				continue;
			}
			size_t len = (size_t(rx_[4]) << 8) | rx_[5];
			if (len > 1024) {
				LOGW("uart: absurd frame length %zu, resyncing", len);
				rx_.erase(rx_.begin());
				continue;
			}
			size_t total = 6 + len + 1;
			if (rx_.size() < total)
				break;
			uint8_t sum = 0;
			for (size_t i = 0; i < total - 1; i++)
				sum += rx_[i];
			if (sum != rx_[total - 1]) {
				LOGW("uart: bad checksum: %s", hex(rx_.data(), total).c_str());
				rx_.erase(rx_.begin());
				continue;
			}
			Frame fr{rx_[3], std::vector<uint8_t>(rx_.begin() + 6, rx_.begin() + 6 + len)};
			if (fr.cmd != CMD_HEARTBEAT)
				LOGD("MCU> %s", hex(rx_.data(), total).c_str());
			out.push_back(std::move(fr));
			rx_.erase(rx_.begin(), rx_.begin() + total);
		}
		return true;
	}

private:
	int fd_ = -1;
	std::vector<uint8_t> rx_;
};

// ---------------------------------------------------------------- daemon

volatile sig_atomic_t g_stop = 0;
void on_signal(int) { g_stop = 1; }

class Feederd {
public:
	explicit Feederd(const Config &c) : cfg_(c) {}

	bool init() {
		load_state();
		if (!mcu_.open_port(cfg_.serial))
			return false;
		mosquitto_lib_init();
		m_ = mosquitto_new(cfg_.mqtt_client_id.c_str(), true, this);
		if (!m_) {
			LOGE("mosquitto_new failed");
			return false;
		}
		if (!cfg_.mqtt_user.empty())
			mosquitto_username_pw_set(m_, cfg_.mqtt_user.c_str(),
						  cfg_.mqtt_pass.empty() ? nullptr : cfg_.mqtt_pass.c_str());
		std::string will = topic("online");
		mosquitto_will_set(m_, will.c_str(), 7, "offline", 1, true);
		mosquitto_connect_callback_set(m_, [](mosquitto *, void *o, int rc) { static_cast<Feederd *>(o)->on_connect(rc); });
		mosquitto_disconnect_callback_set(m_, [](mosquitto *, void *o, int rc) { static_cast<Feederd *>(o)->on_disconnect(rc); });
		mosquitto_message_callback_set(m_, [](mosquitto *, void *o, const mosquitto_message *msg) { static_cast<Feederd *>(o)->on_message(msg); });

		// Take over the MCU: LED to the idle mode, ask for every DP's state.
		send_led();
		mcu_.send(CMD_QUERY);
		last_mcu_ms_ = now_ms();
		mqtt_connect();
		return true;
	}

	int run() {
		while (!g_stop) {
			pollfd p[2];
			int n = 0;
			p[n++] = {mcu_.fd(), POLLIN, 0};
			int ms = mqtt_ok_ ? mosquitto_socket(m_) : -1;
			if (ms >= 0)
				p[n++] = {ms, short(POLLIN | (mosquitto_want_write(m_) ? POLLOUT : 0)), 0};
			int r = poll(p, nfds_t(n), 200);
			if (r < 0 && errno != EINTR) {
				LOGE("poll: %s", strerror(errno));
				return 1;
			}
			if (r > 0 && (p[0].revents & POLLIN))
				if (!handle_uart())
					return 1;
			if (n > 1 && r > 0) {
				if (p[1].revents & (POLLIN | POLLERR | POLLHUP))
					check_mqtt(mosquitto_loop_read(m_, 1));
				if (mqtt_ok_ && (p[1].revents & POLLOUT))
					check_mqtt(mosquitto_loop_write(m_, 1));
			}
			if (mqtt_ok_)
				check_mqtt(mosquitto_loop_misc(m_));
			tick();
		}
		LOGI("stopping");
		if (mqtt_ok_ && connected_) {
			pub("online", "offline");
			mosquitto_loop_write(m_, 1);
			mosquitto_disconnect(m_);
		}
		return 0;
	}

private:
	// ---------------- state file (lifetime + daily totals)

	void load_state() {
		FILE *f = fopen(cfg_.state_file.c_str(), "r");
		if (!f) {
			LOGW("no state file %s (%s): totals start at 0 unless the broker remembers them",
			     cfg_.state_file.c_str(), strerror(errno));
			return;
		}
		char line[128];
		while (fgets(line, sizeof line, f)) {
			std::string l = trim(line);
			size_t eq = l.find('=');
			if (eq == std::string::npos)
				continue;
			std::string k = l.substr(0, eq), v = l.substr(eq + 1);
			long iv;
			if (k == "total" && parse_int(v, iv)) total_ = iv;
			else if (k == "today" && parse_int(v, iv)) today_ = iv;
			else if (k == "day") day_ = v;
		}
		fclose(f);
		state_from_file_ = true;
		LOGI("state: total=%ld today=%ld day=%s", total_, today_, day_.c_str());
	}

	void save_state() {
		std::string tmp = cfg_.state_file + ".tmp";
		FILE *f = fopen(tmp.c_str(), "w");
		if (!f) {
			LOGE("save state %s: %s", tmp.c_str(), strerror(errno));
			return;
		}
		fprintf(f, "total=%ld\ntoday=%ld\nday=%s\n", total_, today_, day_.c_str());
		fflush(f);
		fsync(fileno(f));
		fclose(f);
		if (rename(tmp.c_str(), cfg_.state_file.c_str()) < 0)
			LOGE("rename %s: %s", tmp.c_str(), strerror(errno));
	}

	// The feeding day starts at day_reset_hour, so 01:00 counts as yesterday.
	// Empty while the clock isn't set yet (no NTP), so we never reset on 1970.
	std::string feeding_day() const {
		time_t t = time(nullptr);
		struct tm lt;
		localtime_r(&t, &lt);
		if (lt.tm_year + 1900 < 2026)
			return "";
		t -= time_t(cfg_.day_reset_hour) * 3600;
		localtime_r(&t, &lt);
		char b[16];
		strftime(b, sizeof b, "%Y-%m-%d", &lt);
		return b;
	}

	static std::string iso_now() {
		time_t t = time(nullptr);
		struct tm lt;
		localtime_r(&t, &lt);
		char b[40];
		strftime(b, sizeof b, "%Y-%m-%dT%H:%M:%S%z", &lt);
		std::string s = b; // +0100 -> +01:00
		if (s.size() >= 5)
			s.insert(s.size() - 2, ":");
		return s;
	}

	// ---------------- MQTT

	std::string topic(const char *leaf) const { return cfg_.root_topic + "/" + leaf; }

	void mqtt_connect() {
		int rc = mosquitto_connect(m_, cfg_.mqtt_host.c_str(), cfg_.mqtt_port, 30);
		if (rc != MOSQ_ERR_SUCCESS) {
			LOGW("mqtt connect %s:%d: %s (retrying)", cfg_.mqtt_host.c_str(), cfg_.mqtt_port,
			     rc == MOSQ_ERR_ERRNO ? strerror(errno) : mosquitto_strerror(rc));
			mqtt_ok_ = false;
			retry_at_ = now_ms() + backoff_;
			backoff_ = backoff_ * 2 > 60000 ? 60000 : backoff_ * 2;
			return;
		}
		mqtt_ok_ = true;
	}

	void check_mqtt(int rc) {
		if (rc == MOSQ_ERR_SUCCESS)
			return;
		if (mqtt_ok_)
			LOGW("mqtt connection lost: %s", rc == MOSQ_ERR_ERRNO ? strerror(errno) : mosquitto_strerror(rc));
		mqtt_ok_ = false;
		connected_ = false;
		retry_at_ = now_ms() + backoff_;
		backoff_ = backoff_ * 2 > 60000 ? 60000 : backoff_ * 2;
	}

	void on_connect(int rc) {
		if (rc != 0) {
			LOGE("mqtt refused: %s", mosquitto_connack_string(rc));
			check_mqtt(MOSQ_ERR_CONN_REFUSED);
			return;
		}
		LOGI("mqtt connected to %s:%d", cfg_.mqtt_host.c_str(), cfg_.mqtt_port);
		connected_ = true;
		backoff_ = 1000;
		echo_.clear();
		const char *subs[] = {"feed", "led", "food", "aux-button", "last-feed", "portions-total", "portions-today", "online"};
		for (auto s : subs) {
			std::string t = topic(s);
			mosquitto_subscribe(m_, nullptr, t.c_str(), 0);
		}
		// Without a state file (fresh flash/OTA wipes the overlay), the
		// broker's retained lifetime total is the backup: adopt it if it
		// arrives within a few seconds of the first connect. (Today's count
		// isn't adopted: it may be from another day.)
		if (!state_from_file_ && !adopt_tried_) {
			adopt_tried_ = true;
			adopt_until_ = now_ms() + 3000;
		}
		publish_all(); // totals wait for the adopt window, so graphs never see a dip to 0
	}

	void on_disconnect(int rc) {
		connected_ = false;
		if (rc != 0)
			check_mqtt(MOSQ_ERR_CONN_LOST);
	}

	// Every topic is two-way. Before publishing a status we note the payload,
	// and the echo the broker sends back to us is skipped.
	void pub(const char *leaf, const std::string &payload) {
		std::string t = topic(leaf);
		if (!connected_)
			return;
		auto &q = echo_[t];
		q.push_back(payload);
		if (q.size() > 16)
			q.pop_front();
		int rc = mosquitto_publish(m_, nullptr, t.c_str(), int(payload.size()), payload.data(), 0, true);
		if (rc != MOSQ_ERR_SUCCESS)
			LOGW("publish %s: %s", t.c_str(), mosquitto_strerror(rc));
		LOGD("pub %s = %s", t.c_str(), payload.c_str());
	}

	void publish_all() {
		pub("online", "online");
		pub("feed", std::to_string(feeding_ ? feed_req_ : 0));
		pub("led", led_mode_);
		if (food_known_)
			pub("food", food_low_ ? "low" : "ok");
		pub("aux-button", aux_);
		if (!last_feed_.empty())
			pub("last-feed", last_feed_);
		if (!adopt_until_) {
			pub("portions-total", std::to_string(total_));
			pub("portions-today", std::to_string(today_));
		}
	}

	void on_message(const mosquitto_message *msg) {
		std::string t = msg->topic;
		std::string p(static_cast<const char *>(msg->payload), size_t(msg->payloadlen));
		if (msg->retain) {
			// Replayed on subscribe: old state, never a command.
			if (adopt_until_ && now_ms() < adopt_until_)
				adopt_retained(t, p);
			return;
		}
		auto it = echo_.find(t);
		if (it != echo_.end()) {
			auto &q = it->second;
			for (auto e = q.begin(); e != q.end(); ++e)
				if (*e == p) {
					q.erase(q.begin(), e + 1);
					return;
				}
		}
		std::string leaf = t.substr(cfg_.root_topic.size() + 1);
		p = trim(p);
		LOGI("mqtt in: %s = %s", leaf.c_str(), p.c_str());
		if (leaf == "feed")
			cmd_feed(p);
		else if (leaf == "led")
			cmd_led(p);
		else
			correct(leaf);
	}

	void adopt_retained(const std::string &t, const std::string &p) {
		long v;
		if (!parse_int(trim(p), v) || v < 0)
			return;
		if (t != topic("portions-total") || v <= total_)
			return;
		LOGW("no state file: adopting retained portions-total=%ld from the broker", v);
		total_ = v;
		save_state();
	}

	// Someone wrote a value that isn't true (or a read-only topic): put the
	// real state back so the topic never lies.
	void correct(const std::string &leaf) {
		LOGW("rejected write to %s, republishing the real state", leaf.c_str());
		if (leaf == "feed") pub("feed", std::to_string(feeding_ ? feed_req_ : 0));
		else if (leaf == "led") pub("led", led_mode_);
		else if (leaf == "food") { if (food_known_) pub("food", food_low_ ? "low" : "ok"); }
		else if (leaf == "aux-button") pub("aux-button", aux_);
		else if (leaf == "last-feed") { if (!last_feed_.empty()) pub("last-feed", last_feed_); }
		else if (leaf == "portions-total") pub("portions-total", std::to_string(total_));
		else if (leaf == "portions-today") pub("portions-today", std::to_string(today_));
		else if (leaf == "online") pub("online", "online");
	}

	// ---------------- commands

	void cmd_feed(const std::string &p) {
		long n;
		if (!parse_int(p, n)) {
			correct("feed");
			return;
		}
		if (n == 0 && !feeding_)
			return; // already idle
		if (feeding_) {
			LOGW("feed %ld refused: still dispensing %d", n, feed_req_);
			correct("feed");
			return;
		}
		if (n < 1 || n > cfg_.max_portions) {
			LOGW("feed %ld refused: allowed 1..%d", n, cfg_.max_portions);
			correct("feed");
			return;
		}
		start_feed(int(n), "mqtt");
	}

	void start_feed(int n, const char *source) {
		LOGI("feeding %d portion(s) (%s)", n, source);
		feeding_ = true;
		feed_req_ = n;
		feed_source_ = source;
		// ~0.6 s for one portion and ~8 s for five on the bench; be generous.
		feed_deadline_ = now_ms() + 15000 + int64_t(n) * 3000;
		mcu_.set_dp_value(DP_FEED, uint32_t(n));
		pub("feed", std::to_string(n));
	}

	void finish_feed(int requested, int dispensed, const char *source, const char *error) {
		char b[200];
		snprintf(b, sizeof b, "{\"requested\":%d,\"dispensed\":%d,\"source\":\"%s\",\"at\":\"%s\"%s%s%s}",
			 requested, dispensed, source, iso_now().c_str(),
			 error ? ",\"error\":\"" : "", error ? error : "", error ? "\"" : "");
		last_feed_ = b;
		if (dispensed > 0) {
			roll_day();
			total_ += dispensed;
			today_ += dispensed;
			save_state();
		}
		if (error)
			LOGE("feed: %s", last_feed_.c_str());
		else
			LOGI("feed done: %s", last_feed_.c_str());
		pub("feed", "0");
		pub("last-feed", last_feed_);
		pub("portions-total", std::to_string(total_));
		pub("portions-today", std::to_string(today_));
	}

	void cmd_led(const std::string &p) {
		if (!led_mode_valid(p)) {
			correct("led");
			return;
		}
		led_mode_ = p;
		send_led();
		pub("led", led_mode_);
	}

	void send_led() {
		std::string eff = led_mode_ == "idle" ? cfg_.led_idle : led_mode_;
		uint8_t s = led_status_for(eff);
		mcu_.send(CMD_NET_STATUS, &s, 1);
	}

	// ---------------- MCU frames

	bool handle_uart() {
		std::vector<Mcu::Frame> frames;
		if (!mcu_.read_frames(frames))
			return false;
		for (auto &f : frames) {
			last_mcu_ms_ = now_ms();
			if (mcu_silent_) {
				LOGI("MCU is talking again");
				mcu_silent_ = false;
			}
			switch (f.cmd) {
			case CMD_HEARTBEAT:
				mcu_.send(CMD_HEARTBEAT);
				break;
			case CMD_TIME:
				send_time();
				break;
			case CMD_DP_REPORT:
				parse_dps(f.data);
				break;
			case CMD_NET_STATUS:
			case CMD_SET_DP:
				break; // acks of ours
			default:
				LOGW("MCU sent unhandled cmd 0x%02x: %s", f.cmd, hex(f.data.data(), f.data.size()).c_str());
			}
		}
		return true;
	}

	void send_time() {
		time_t t = time(nullptr);
		struct tm lt;
		localtime_r(&t, &lt);
		if (lt.tm_year + 1900 < 2026) {
			uint8_t d[] = {0x00, 0, 0, 0, 0, 0, 0, 0};
			mcu_.send(CMD_TIME, d, sizeof d); // 00 = time not valid yet
			return;
		}
		// Same layout the stock firmware sent: 01 YY MM DD hh mm ss weekday(Sun=1).
		uint8_t d[] = {0x01, uint8_t(lt.tm_year - 100), uint8_t(lt.tm_mon + 1), uint8_t(lt.tm_mday),
			       uint8_t(lt.tm_hour), uint8_t(lt.tm_min), uint8_t(lt.tm_sec), uint8_t(lt.tm_wday + 1)};
		mcu_.send(CMD_TIME, d, sizeof d);
	}

	void parse_dps(const std::vector<uint8_t> &d) {
		size_t i = 0;
		while (i + 4 <= d.size()) {
			uint8_t dp = d[i], type = d[i + 1];
			size_t len = (size_t(d[i + 2]) << 8) | d[i + 3];
			if (i + 4 + len > d.size()) {
				LOGW("DP %u: truncated report", dp);
				return;
			}
			uint32_t v = 0;
			for (size_t k = 0; k < len && k < 4; k++)
				v = (v << 8) | d[i + 4 + k];
			on_dp(dp, type, v);
			i += 4 + len;
		}
	}

	void on_dp(uint8_t dp, uint8_t type, uint32_t v) {
		switch (dp) {
		case DP_FEED:
			LOGD("MCU accepted feed %u", v);
			break;
		case DP_DISPENSED:
			if (feeding_) {
				int req = feed_req_;
				feeding_ = false;
				finish_feed(req, int(v), feed_source_, int(v) < req ? "short" : nullptr);
			} else {
				// A feed button on the device: the MCU fed by itself.
				pub("feed", std::to_string(v));
				finish_feed(int(v), int(v), "button", nullptr);
			}
			break;
		case DP_FAULT: {
			bool low = v & kFaultFoodLow;
			if (v & ~uint32_t(kFaultFoodLow))
				LOGW("MCU fault bits 0x%02x (unknown besides food-low)", v);
			if (!food_known_ || low != food_low_) {
				bool was_low = food_known_ && food_low_;
				food_known_ = true;
				food_low_ = low;
				LOGI("food %s", low ? "low" : "ok");
				pub("food", low ? "low" : "ok");
				if (was_low && !low)
					send_led(); // in case the alarm dropped our pattern
			}
			break;
		}
		case DP_AUX_SHORT:
			// Real presses hold 1 for ~0.6 s; the MCU also blips 1->0 in
			// ~40 ms on its own around feeds. Only a long 1 is a press.
			if (v) {
				aux_down_ms_ = now_ms();
			} else if (aux_down_ms_) {
				int64_t held = now_ms() - aux_down_ms_;
				aux_down_ms_ = 0;
				if (held >= 250) {
					LOGI("aux button pressed (%lld ms)", (long long)held);
					pub("aux-button", "pressed");
					pub("aux-button", "idle");
				} else
					LOGD("DP 240 blip %lld ms, not a press", (long long)held);
			}
			break;
		case DP_AUX_HELD: {
			std::string a = v ? "held" : "idle";
			if (a != aux_) {
				aux_ = a;
				LOGI("aux button %s", a.c_str());
				pub("aux-button", aux_);
			}
			break;
		}
		default:
			LOGD("DP %u type %u = %u (ignored)", dp, type, v);
		}
	}

	// ---------------- timers

	void roll_day() {
		std::string d = feeding_day();
		if (d.empty() || d == day_)
			return;
		if (!day_.empty())
			LOGI("new feeding day %s (yesterday: %ld portions)", d.c_str(), today_);
		day_ = d;
		bool changed = today_ != 0;
		today_ = 0;
		save_state();
		if (changed)
			pub("portions-today", "0");
	}

	void tick() {
		int64_t now = now_ms();
		if (feeding_ && now > feed_deadline_) {
			feeding_ = false;
			finish_feed(feed_req_, 0, feed_source_, "timeout");
		}
		if (!mcu_silent_ && now - last_mcu_ms_ > 5000) {
			LOGE("MCU silent for 5 s (no heartbeat)");
			mcu_silent_ = true;
		}
		if (adopt_until_ && now >= adopt_until_) {
			adopt_until_ = 0;
			pub("portions-total", std::to_string(total_));
			pub("portions-today", std::to_string(today_));
		}
		if (now - last_day_check_ > 1000) {
			last_day_check_ = now;
			roll_day();
		}
		if (!mqtt_ok_ && now >= retry_at_) {
			if (retry_at_)
				LOGI("mqtt reconnecting");
			if (m_ && retry_at_) {
				int rc = mosquitto_reconnect(m_);
				if (rc == MOSQ_ERR_SUCCESS)
					mqtt_ok_ = true;
				else
					check_mqtt(rc);
			} else
				mqtt_connect();
		}
	}

	Config cfg_;
	Mcu mcu_;
	mosquitto *m_ = nullptr;
	bool mqtt_ok_ = false;   // socket up (connect issued)
	bool connected_ = false; // CONNACK received
	int64_t retry_at_ = 0;
	int64_t backoff_ = 1000;
	std::map<std::string, std::deque<std::string>> echo_;
	int64_t adopt_until_ = 0;

	bool feeding_ = false;
	int feed_req_ = 0;
	const char *feed_source_ = "mqtt";
	int64_t feed_deadline_ = 0;
	std::string last_feed_;

	std::string led_mode_ = "idle";
	bool food_known_ = false, food_low_ = false;
	std::string aux_ = "idle";
	int64_t aux_down_ms_ = 0;

	long total_ = 0, today_ = 0;
	std::string day_;
	bool state_from_file_ = false, adopt_tried_ = false;
	int64_t last_day_check_ = 0;

	int64_t last_mcu_ms_ = 0;
	bool mcu_silent_ = false;
};

} // namespace

int main(int argc, char **argv) {
	const char *conf = "/etc/feederd.conf";
	int opt;
	while ((opt = getopt(argc, argv, "c:vf")) != -1) {
		switch (opt) {
		case 'c': conf = optarg; break;
		case 'v': g_verbose = true; break;
		case 'f': g_stderr = true; break;
		default:
			fprintf(stderr, "usage: %s [-c config] [-v verbose] [-f log to stderr]\n", argv[0]);
			return 2;
		}
	}
	openlog("feederd", LOG_PID, LOG_DAEMON);
	Config cfg;
	if (!load_config(conf, cfg))
		return 1;
	signal(SIGINT, on_signal);
	signal(SIGTERM, on_signal);
	signal(SIGPIPE, SIG_IGN);
	LOGI("starting: broker %s:%d, root %s, led idle=%s, max %d portions",
	     cfg.mqtt_host.c_str(), cfg.mqtt_port, cfg.root_topic.c_str(), cfg.led_idle.c_str(), cfg.max_portions);
	Feederd d(cfg);
	if (!d.init())
		return 1;
	return d.run();
}
