## Recommended tools for operation

* Arduino IDE with ESP32 boards (package by Espressif, not the Arduino one) and the Adafruit_VS1053 library installed
* Philips screwdriver(s)
* SD Card slot/reader
* Data-enabled USB type C cable

## Quick Setup
Setting up the phone is quite simple. The only configuration that needs to be done is specifying WiFi credentials to be used, and a webhook URL to upload to. Both these configurations are done through files on the SD card.

Once you have the SD card plugged into your computer, you can edit the following files:

* <b>wifi.txt</b><br>
* <b>webhook.txt</b>

For how to fill in these files, see [WiFi](#wifi) and [Webhook](#webhook) respectively.

## Software
The ESP32 has already been flashed with the software it needs. In the case it is necessary to change something and/or reflash the ESP32, the source Arduino IDE project can be found [here](https://github.com/mFieldHouses/n8n-phonecorder).

The phone runs on an ESP32 module, the <b>Beetle ESP32-C6</b> by DFRobot. When flashing a new program, make sure that board has been selected. It should come with the ESP32 boards package by Espressif Systems. If Serial communication with the Beetle via the Serial monitor is needed, make sure “USB CDC on boot” is enabled in the Tools menu in the Arduino IDE. No other settings need to be changed.

The “Adafruit VS1053” library is required for this project to compile correctly. Install it if you do not have it yet.

## SD card
The SD card must be formatted in FAT32 format.

The software expects a certain file structure on the SD card. Do not rename or remove any of the files and folders that are already present on the card.
The required file structure is as follows:
	
* /<b>recorded</b>: (Folder in which all recorded feedback OGG files will be stored)
  * /<b>uploaded</b> (All OGG files that have successfully been uploaded to the webhook will be placed here) 
  * (All OGG files that have not successfully been uploaded to the webhook will stay in this directory)
* /<b>sounds</b>: (Folder in which all prerecorded sound files are stored)
  * <b>pickup.mp3</b> (File to be played when the phone is picked up. When playback of this file finishes, recording is started. Can be changed to any .mp3 file)
  * <b>restarting.mp3</b> (File to be played when the ESP will restart itself due to an error)
  * <b>*_error.mp3</b> (Files to be played when certain types of errors arise. Usually followed by restarting.mp3.)
* /wifi.txt (Configures WiFi connection credentials. See [WiFi](#wifi))
* /webhook.txt (Provides the phone with a webhook URL to upload files to. See [Webhook](#webhook))
* /recording_profile.img (OGG recording profile/plugin. See [Recording](#recording))

## Status API
When running, the phone exposes an API endpoint which will return data about its status, in JSON format. This API is available both at _\<hostname\>_ and at _\<hostname\>/api/status_.

A status message follows a certain structure:

```
{
  “status”: “idle” | “in_use” | ”uploading”,
  “errors”: [<list of strings describing errors>],
  “used_space_percentage”: <float>,
  “total_recordings_count”: <int>,
  “uploaded_recordings_count”: <int>
}
```

When the phone is playing or recording sound, it will not generate the full status report, as doing so can cause audible hiccups in playback and recording.

## Output Log
The phone publishes a simple copy of its serial output to _\<hostname\>/output_. This log contains some additional information about the phones’ functioning.

## Webhook
The phone will try to upload its recorded files to a webhook via HTTP POST requests. You can provide the phone with a link to the webhook in <b>webhook.txt</b>. 

This file is expected to have a single complete URL on its first line. Anything after the first line will be ignored.

The phone also expects a 200 response code. If it does not receive this, it will show the “Webhook not responding with OK code” error (See [Errors](#errrors) and [Status API](#status-api)), and any file upload that gets this response will not be marked as properly completed and will be queued for reattempt.

## WiFi
When the phone powers up it will try to connect to a WiFi network using the credentials specified in <b>wifi.txt</b> on the SD Card. This file must consist of two lines and must be structured exactly as follows:

```
ssid=<wifi network name>
password=<wifi network password>
```

(Angle brackets and whatever they contain may be replaced by anything else.)

Make sure that there are no extra unwanted spaces in either line, since those will be parsed and included in the credentials too.

## Recording
The phone is of course designed to record audio snippets to OGG files. This is done using the VS1053 and a “recording plugin”. This plugin needs to be present on the SD card (See [SD card](#sd_card)), under the name “recording_profile.img”. 

You can download profiles to install through [this link](https://www.vlsi.fi/fileadmin/software/VS10XX/vs1053-vorbis-encoder-170c.zip). By default, the profile on the SD card is the v44k1q05 profile; it will record at 44khz. There are profiles available that record at lower sample rates, like 16khz and 8khz, if smaller OGG files are required. More information about these profiles is available on [this page](https://www.vlsi.fi/en/support/software/vs10xxapplications.html). (Scroll down to the header “VS1053 Ogg Vorbis Encoder Application”)

## Errors
When the phone encounters an error, it will play a sound snippet according to what error occurred. Here is a full list of possible errors and their sounds:

* **Single beep**: SD Card error. Is the SD card inserted in the SD card slot on the VS1053 breakout board?

* “**Recording Plugin Error**”: OGG Recording plugin error. Is the plugin file in the right location and is it named correctly? (See [SD Card](#sd-card))

* “**WiFi Error**”: WiFi error. Have the WiFi credentials been correctly specified in wifi.txt? Does the phone have access to the specified network? (See [SD Card](#sd-card) and [WiFi](#wifi))

* “**Webhook Error**”: Webhook error. The webhook is not returning success codes (or anything at all). Is the webhook accessible? Has the webhook URL been written correctly in <b>webhook.txt</b>? (See [SD Card](#sd-card) and [Webhook](#webhook)) 

Further error diagnosis can be done by viewing/retrieving the status of the phone via the API. (See [Status API](#status-api))

## Troubleshooting
There are a few errors that could happen which the phone cannot resolve itself.

If the phone does not seem to function right, e.g. if it doesn’t respond to web requests anymore, the first step is to hook the phone’s USB-C cable to your laptop and to monitor the Serial output using the Arduino IDE.
If the serial output does not clarify the issue, you can see if your issue is in the following list of known issues:

1. Phone keeps stalling when uploading file

This can be caused by the phone trying to upload a bad file. Check for all un-uploaded files and see if there are any obviously corrupt/bad ones, for example files that are 0B in size.
