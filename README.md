# ddcci_dongle

An RP2040-powered dongle to be plugged between your computer and the monitor.

Continuation of the thread of thought of [dvi_i2c_checkpoint](https://github.com/parport0/dvi_i2c_checkpoint) + [ddcci_injector](https://github.com/parport0/ddcci_injector).

This dongle should allow you, given a functioning firmware, to inject your DDC/CI sequences into a video cable.

There is an I2C channel broken out into the 3.5mm jack (GND/SDA/SCK/3V3). This should allow a theoretical user to have a control button or similar mounted close to their fingers, without having to pull heavy video cables onto the desk.

* JLC04161H-7628
* Make sure to select "NanYa NP-155F" as the material when ordering to guarantee impedance control
* HDMI connectors are best bought separately and soldered manually to avoid added PCBA costs

![Front of the PCB](/ddcci_dongle_front.png)

![Back of the PCB](/ddcci_dongle_back.png)

# Firmware

The firmware is a simplification of the [ddcci_injector](https://github.com/parport0/ddcci_injector) firmware.

It assumes there is a Sparkfun Qwiic Twist sitting on the other end, connected to a 4-pin audio jack.

This uses:

* [The arduino-pico core of earlephilhower](https://github.com/earlephilhower/arduino-pico)

```
arduino-cli config add board_manager.additional_urls https://github.com/earlephilhower/arduino-pico/releases/download/global/package_rp2040_index.json
arduino-cli core update-index
arduino-cli core install rp2040:rp2040

arduino-cli compile -e -v --fqbn rp2040:rp2040:rpipico ddcci_dongle_sw.ino
```

The build result will be in `build/rp2040.rp2040.rpipico/ddcci_dongle_sw.ino.uf2`
