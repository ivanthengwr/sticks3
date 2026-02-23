##Goal, to implement microphone recording and speaker playback on the M5stack StickS3 using ESP-IDF

###Utilize the ESP332-S3 I2C and I2S peripherals. 

The device uses an ES8311 mono audio codec and an AW8737 power amplifier.

	Step 1: Define Pin Mappings
		First, define the hardware pins used for the audio system according to the StickS3 pinmap
			1. I2C Pins (Shared with BMI270 and M5PM1): SDA = GPIO 47, SCL = GPIO 48
			2. I2S Pins:
				◦ MCLK (Master Clock) = GPIO 18
				◦ BCLK (Bit Clock) = GPIO 17
				◦ LRCK / WS (Word Select) = GPIO 15
				◦ DOUT (Data from ES8311 to ESP32-S3 RX) = GPIO 14
				◦ DIN (Data from ESP32-S3 TX to ES8311) = GPIO 16
    
	Step 2: Initialize the I2C Bus
		The ES8311 codec and the M5PM1 power management chip are both configured via I2C.
			1. Use 'i2c_param_config()' and 'i2c_driver_install()' to initialize the I2C master on GPIO 47 (SDA) and GPIO 48 (SCL).
			2. Set the I2C clock speed (e.g., 100 kHz or 400 kHz).e

	Step 3: Initialize the Power Amplifier (AW8737)
		The StickS3 uses a companion M5PM1 power management chip to enable the speaker's AW8737 amplifier.
			1. Using your I2C master, send configuration commands to the M5PM1 chip to pull the PYG3_SPK_Pulse line high. This turns on the power amplifier for playback.
			2. Note: If you intend to use the Infrared (IR) receiver simultaneously, you must turn off the speaker amplifier, otherwise IR reception will not work properly.
		
	Step 4: Configure the ES8311 Codec via I2C
		Before the I2S interface can send or receive data, the ES8311 codec must be set up.
			1. Write to the ES8311's I2C registers to bring the chip out of standby mode.
			2. Enable the internal pre-amplifiers for the MEMS microphone.
			3. Route the codec's internal ADC to the I2S output (DOUT) and the I2S input (DIN) to the internal DAC.
			4. Set the audio data format. The StickS3 specifications require setting the ES8311 to use the I2S protocol with a 24-bit resolution.

	Step 5: Initialize the I2S Controller (ESP-IDF v5.x API)
		Configure the ESP32-S3's I2S controller to act as the master, generating the BCLK and WS signals.
			1. Allocate Channels: Use i2s_new_channel() to allocate both a TX (playback) and an RX (recording) channel using the I2S_NUM_0 port. Set the role to I2S_ROLE_MASTER.
			2. Configure Standard Mode: Use i2s_channel_init_std_mode() to initialize the channels:
			    ◦ Clocks: Set the sample rate (e.g., 16 kHz or 44.1 kHz). Enable the MCLK output.
			    ◦ Slot Configuration: Set the data bit-width to 24 bits to match the ES8311 configuration. Set the slot mode to mono.
			    ◦ GPIO Routing: Map mclk to GPIO 18, bclk to GPIO 17, ws to GPIO 15, dout to GPIO 16 (TX), and din to GPIO 14 (RX).
			3. Enable Channels: Call i2s_channel_enable() on both the RX and TX channels.
	Step 6: Implement the Record and Playback Loop
		Since the StickS3 has 8MB of PSRAM built-in, you can dynamically allocate a large buffer to hold your audio recording.
			1. Recording: Allocate a buffer in PSRAM. Use i2s_channel_read() on the RX channel handle to capture the 24-bit audio stream coming from the microphone.
			2. Playback: Use i2s_channel_write() on the TX channel handle to send the captured audio buffer back out to the speaker.

⚠️ Power Management Notice
When testing your playback routine while the device is strictly powered by its internal 250mAh lithium battery (USB disconnected), you must restrict your audio volume in software to below 75%. Driving the speaker at maximum volume can cause excessive power consumption and lead to unexpected device reboots.
