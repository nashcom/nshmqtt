# nshmqtt - lightweight HTTP-to-MQTT gateway and MQTT-to-Prometheus bridge
#
# Requires: g++ with C++17, Eclipse Paho MQTT C (libpaho-mqtt3c, dev headers
# + library), libcurl (dev headers + library, for webhook forwarding),
# pthread. Linux only.

CXX      ?= g++
CXXFLAGS ?= -std=c++17 -Wall -Wextra -O2 -pthread
LDFLAGS  ?= -pthread
LDLIBS   := -lpaho-mqtt3c -lcurl

PREFIX      ?= /usr/local
SBINDIR     := $(PREFIX)/sbin
CONF_DIR    := /etc/nshmqtt

SRC_DIR  := src
BIN      := nshmqtt

SRCS := $(wildcard $(SRC_DIR)/*.cpp)
OBJS := $(SRCS:.cpp=.o)
DEPS := $(OBJS:.o=.d)

TEST_BIN     := tests/test_nshmqtt
TEST_SRCS    := tests/test_nshmqtt.cpp \
                 $(SRC_DIR)/text_util.cpp \
                 $(SRC_DIR)/json_util.cpp \
                 $(SRC_DIR)/config.cpp \
                 $(SRC_DIR)/http.cpp \
                 $(SRC_DIR)/state.cpp \
                 $(SRC_DIR)/metrics.cpp \
                 $(SRC_DIR)/mqtt_topic.cpp \
                 $(SRC_DIR)/webhook_json.cpp \
                 $(SRC_DIR)/event_placeholders.cpp

.PHONY: all clean test install

all: $(BIN)

$(BIN): $(OBJS)
	$(CXX) $(CXXFLAGS) $(LDFLAGS) -o $@ $(OBJS) $(LDLIBS)

$(SRC_DIR)/%.o: $(SRC_DIR)/%.cpp
	$(CXX) $(CXXFLAGS) -MMD -MP -c $< -o $@

-include $(DEPS)

test: $(TEST_BIN)
	./$(TEST_BIN)

$(TEST_BIN): $(TEST_SRCS)
	$(CXX) $(CXXFLAGS) -o $@ $(TEST_SRCS)

clean:
	rm -f $(OBJS) $(DEPS) $(BIN) $(TEST_BIN)

# Installs the binary and a starter config only -- does NOT create the
# nshmqtt user/group, install/enable the systemd service, or touch an
# existing config file.
install: $(BIN)
	install -D -m 0755 $(BIN) $(DESTDIR)$(SBINDIR)/$(BIN)
	install -m 0755 -d $(DESTDIR)$(CONF_DIR)
	install -m 0644 etc/nshmqtt.conf.example \
		$(DESTDIR)$(CONF_DIR)/nshmqtt.conf.example
	@echo
	@echo "Installed $(BIN) to $(DESTDIR)$(SBINDIR)/$(BIN)"
	@echo "See README.md for the systemd unit and NGINX configuration."
