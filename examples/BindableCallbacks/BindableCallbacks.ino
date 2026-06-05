#include <Arduino.h>
#include <Signal.h>
#include <functional>

Signal signal;

enum class AppEvent : uint16_t {
	ButtonPressed,
};

class ButtonHandler {
  public:
	explicit ButtonHandler(Signal &bus) : _bus(bus) {
	}

	void begin() {
		_bus.subscribe(AppEvent::ButtonPressed, std::bind(&ButtonHandler::onButton, this));
	}

  private:
	void onButton() {
		Serial.println("bound button handler");
	}

	Signal &_bus;
};

ButtonHandler handler(signal);

void setup() {
	Serial.begin(115200);
	delay(200);

	SignalResult initResult = signal.init();
	if (!initResult) {
		Serial.println(initResult.message.c_str());
		return;
	}

	handler.begin();
	signal.post(AppEvent::ButtonPressed);
}

void loop() {
	delay(1000);
}
