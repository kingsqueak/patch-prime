# Patch Prime user guide

Patch Prime turns your computer's audio into a small mixing console. This guide covers everything in the app, from the first setup to effects and troubleshooting.

- [The idea in one minute](#the-idea-in-one-minute)
- [First setup](#first-setup)
- [The mixer](#the-mixer)
- [Mix buses and virtual devices](#mix-buses-and-virtual-devices)
- [Routing](#routing)
- [Apps](#apps)
- [Scenes](#scenes)
- [Recording and playback](#recording-and-playback)
- [Cue (headphones)](#cue-headphones)
- [Effects](#effects)
- [Meters](#meters)
- [Audio engine settings](#audio-engine-settings)
- [Unplugging and plugging in devices](#unplugging-and-plugging-in-devices)
- [Recipes](#recipes)
- [Troubleshooting](#troubleshooting)

## The idea in one minute

Patch Prime works in three stages, left to right, like a console:

1. **Inputs**: your microphones and instrument interfaces, plus virtual inputs such as "Desktop" (everything your apps play).
2. **Mix buses**: mixes you build, such as "Stream Mix". Inputs are sent into them, and apps like OBS can record them.
3. **Outputs**: your speakers and headphones.

A strip sends its sound to strips in a later stage: an input to a mix bus or an output, a mix bus to an output. Each send has its own level, so one microphone can go loud into your stream and quiet into your headphones.

The engine runs in the background, so closing the window changes nothing: your routing keeps working, and it all comes back after a reboot.

## First setup

1. Open **Patch Prime** from the application menu.
2. Click **Devices** (top right). You'll see every input and output on your system.
3. Tick the devices you want in the mixer. Each ticked device gets a strip.
4. Optionally type a short label for each one (for example "Mic 1" instead of "Fast Track MKII Pro"). The label is what the mixer shows.
5. Go back to the **Mixer**.

If you want apps to play through Patch Prime by default, click the **star** (Default) on the output or virtual input you want them to use. For your microphone, click the star on its input strip; apps then record your mic *with* its effects.

## The mixer

### A channel strip

From top to bottom:

| Part | What it does |
|---|---|
| Colored bar | The stage: green for inputs, your accent color for mix buses, orange for outputs. |
| Light and name | The light glows while any sound passes, even sound too quiet for the meters. Under the name: the kind (Mono, Stereo, Mix bus, Virtual input, Ch 1–2) or **Offline**. |
| Insert slots | The strip's effects, in order. Lit means on, dim means bypassed. Click any slot (or **+ Effects**) to open the effects window. |
| Send button | Where the strip goes (for example "→ Stream Mix", or "→ 3 places"). Click it to tick destinations. Outputs have no sends. |
| Meters | One per channel. See [Meters](#meters). Mix buses and outputs show their loudness (LUFS) underneath. |
| Fader | The strip's level, from silent (∞) to +12 dB. **Double-click** it for 0 dB. |
| Level readout | The fader's value. **Click** it to type a level, for example `-6` or `-inf`. |
| **M** | Mute (red when on). |
| **C** | Cue: hear this strip alone in your headphones. Only shown once you've chosen a cue output. |
| Microphone | Inputs only. **Mono**: mixes all of this input's channels into one, heard in both ears. Use it for a mic on one input of a stereo interface. |
| Star | Make this strip the system default output (for strips apps play into) or default microphone (for inputs). |
| Channel pair | On multichannel interfaces: use every channel, or only one stereo pair (for example "Ch 3–4"). |
| **Record** | Inputs, mix buses and Desktop: record this strip to a file. While recording, the button turns red and shows the time; click it to stop. Next to a channel pair menu it's just a dot. See [Recording and playback](#recording-and-playback). |

On a hardware **input**, effects run *before* the fader, as on a console, so apps recording your mic get the processed sound. On mix buses and outputs they run *after* the fader, which is what a limiter wants.

The mixer scrolls sideways when there are more strips than fit.

### The top row

- **Scene**: save and load the whole mix. See [Scenes](#scenes).
- **Cue**: choose your headphones for cue. See [Cue](#cue-headphones).

### Menus and shortcuts

- **File → Quit** (Ctrl+Q) closes the window. The engine keeps running, so your audio doesn't stop.
- **View** switches pages: **Mixer** (Ctrl+1), **Apps** (Ctrl+2), **Routing** (Ctrl+3), **Devices** (Ctrl+4), **Recordings** (Ctrl+5).
- **Help → Patch Prime User Guide** (F1) opens this guide in a window of its own, built into the app so it works offline; **User Guide Online** opens it on GitHub; **Report a Bug…** opens the issue tracker; **About Patch Prime** shows the version and license.

## Mix buses and virtual devices

On the **Devices** page, **Add Virtual Device…** makes one of three kinds:

- **Input**: apps play into it, and you route it like an input. Example: "Desktop", carrying all system sound to several outputs at once.
- **Mix bus**: inputs and apps mix into it, it goes to outputs, and apps can record it. Example: "Stream Mix" for OBS, or a "Reverb" bus.
- **Virtual microphone**: apps record from it like a mic. Example: "Chat Mic" for Discord.

Virtual devices show in other apps under readable names such as `patchprime.stream-mix`. Delete one with the **✕** next to it on the Devices page.

## Routing

The **Routing** page shows every send at once. Rows are the strips that send (inputs, then mix buses); columns are the ones that receive (mix buses, then outputs).

- Click **+** to send a row to a column; it becomes a lit **✓**. Click again to stop.
- Under a lit send: a slider for that send's level, and the level in dB (click it to type one).
- Sends only go to a later stage, so a mix bus row only has buttons under outputs.

## Apps

The **Apps** page lists applications that play or record sound, running or not.

- Pick where each app plays to (or records from): a strip, or "System default".
- The choice is remembered and applied whenever the app starts.
- Apps that aren't running have a **✕** to forget them.
- If an app chooses its own device in its settings (Audacity's device setting, for example), that choice wins and is remembered as its route.

## Scenes

A scene is the whole mix saved under a name:

- every strip's level, mute, mono, sends and send levels;
- its effects and their settings;
- the app routes;
- the default output and microphone.

Devices, labels and engine settings are not part of scenes.

- **Save…** stores the current mix. Typing an existing name replaces that scene.
- **Load** sets the mix to the chosen scene, without restarting anything.
- **✕** deletes the chosen scene. The mix stays as it is.

## Recording and playback

Inputs, mix buses (Stream Mix, for example) and routed inputs (Desktop) can be recorded to a file. Press **Record** at the bottom of the strip, and press it again to stop. Several strips can record at once.

- **What's recorded** on a mix bus or Desktop is what the strip sends on, after its effects and its fader: what the rest of the mix hears from it, and what OBS would capture. On a hardware **input** (a microphone) it's the raw sound, as the device sends it, before the strip's effects and fader. A mono input records in mono (the same sound in both channels).
- **Where:** files go to `~/Music/Patch Prime`, named after the strip and the time, for example `Stream Mix 2026-10-31 11-42-07.flac`.
- **Format:** choose it on the Recordings page. **FLAC** is lossless at about half the size of WAV. **WAV** is uncompressed. **MP3** is small and plays everywhere, but only up to 48 kHz: at a higher mixer rate an MP3 recording stops at once with a message. FLAC and WAV are 24-bit. The format applies to the next recording.
- Recording runs in the engine, so it carries on with the window closed. It stops if you change the mixer's sample rate or the audio server restarts; what was recorded until then is kept.

The **Recordings** page (View menu, Ctrl+5, or the Recordings button above the mixer) lists the files, newest first, with their length, size and date.

- **Play** (or double-click a file) plays it through the player. **Play into** chooses the strip it plays into: an input, a mix bus, Desktop or a hardware output. Play into Stream Mix to let your stream hear it, or into your headphones' output to listen alone.
- The player bar has play/pause, **loop** (play the file over and over), stop and a position slider (drag it to jump).

### Setting up a mic's effects on a recording

You can't really hear your own voice while you talk, so tuning a gate or a compressor live is guesswork. Instead:

1. Press **Record** on the mic's strip and talk for half a minute the way you normally do, then stop. This records the mic raw, before its effects.
2. On the Recordings page, choose the mic in **Play into** (it reads "MAMic1 (instead of the device)", for example), turn on **loop**, and play the recording.
3. While it plays, the mic's strip hears the recording *instead of* the microphone: the device is cut off, and the strip's status line reads **▶ Playback**. The recording goes through the strip's whole effects chain, its fader and its sends, exactly as your voice would.
4. Open the strip's effects and adjust them while you listen (cue the strip, or listen on the output it's sent to). Stop the player when you're done: the microphone is connected again at once.
- **Move to Trash** puts a file in the desktop's trash, from which you can still restore it.
- **Open Folder** opens the recordings folder in the file manager.

## Cue (headphones)

Cue lets you listen to a strip in your headphones without changing what goes to your stream or speakers.

1. In the mixer's top row, set **Cue** to your headphones (any hardware output).
2. Every strip now has a **C** button. Click it on the strips you want to hear.
3. While anything is cued, the headphones play only the cued strips. Their own mix comes back when you un-cue everything.

Cue is temporary: it isn't saved, and it isn't part of scenes. Set **Cue** to **Off** to turn it off and clear every cue.

## Effects

Click a strip's insert slots to open its effects window. The window is separate from the mixer, so you can keep both in view.

- **Add Effect** adds an effect at the end of the chain. A strip can have up to eight.
- Effects run top to bottom. Move them with the up and down arrows; remove one with **✕**.
- The **power button** on each effect turns it on or bypasses it, crossfading so it doesn't click.
- Every value can be dragged or typed: click a value box, type (for example `2.5k`, `-6`, `40 ms`), and press Enter. Double-click a slider to reset it.
- A strip without effects has no processing at all. Effects are saved with the strip and in scenes.
- Each effect has a **meter bridge** on its right: **IN** and **OUT** levels (one bar per channel, left and right on stereo), with the peak in dBFS underneath, and between them **GR**, the gain reduction, for the effects that turn the sound down (gate, ducker, multiband compressor, limiter). The leveler shows **GAIN** instead, centred on 0 dB: up when it boosts, down when it cuts. Comparing IN and OUT shows what an effect does to the level, an EQ or reverb included. The scale reaches down to −80 dB, low enough to see a microphone's noise floor. Where an effect compares the level with a setting, the setting is a white line across the meter (hover it for the value): the gate's **Threshold** on IN, the limiter's **Ceiling** on IN and OUT, and the ducker's **Threshold** on its **KEY** meter, the level of the strip it listens to. See [Meters](#meters).

### Parametric EQ

Six bands. Each band can be a bell, a low or high shelf, or a low or high cut (12 dB per octave).

- **On the curve:** drag a numbered handle left and right for frequency, and up and down for gain. Scroll on it to make the band narrower or wider (Q). Double-click it to set its gain to 0 dB.
- **In the table:** each band's on/off, type, frequency, gain and Q.

A new EQ is flat: a low shelf at 80 Hz, bells at 250 Hz, 1 kHz, 3 kHz and 6 kHz, and a high shelf at 12 kHz.

### Multiband compressor

Splits the sound into four bands at three crossovers (120 Hz, 1 kHz and 6 kHz to start) and compresses each band separately.

- Drag the lines on the frequency strip to move the crossovers, or type them.
- Per band: threshold, ratio, attack, release and makeup gain; **Band n** turns a band's compression off; **S** solos a band, so you hear only the soloed ones while setting them up.
- Each band's gain reduction shows as a bar and in dB; the GR meter on the right shows the deepest band.

With nothing compressing, the four bands add back up to the original sound exactly.

### Noise gate

Turns the strip down by **Range** when it falls below **Threshold**, so breaths, keyboard and room noise between words don't come through. **Attack**, **Hold** and **Release** set how fast it opens, how long it stays open after you stop, and how fast it closes. The GR meter shows how much it's cutting. To set it, watch IN while you're quiet and while you talk: put the threshold line between the two.

### Noise suppression

Takes fans, hum and room noise out of a voice with RNNoise, a small neural network. **Amount** blends the cleaned voice with the original; 100 % is fully cleaned. The **Voice** meter shows how sure it is that it hears a voice. It adds 10 ms of delay and works at 48 kHz (at other sample rates it passes the sound through untouched).

### Ducker

Turns this strip down by **Depth** while another strip, its **Key**, is above **Threshold**. Use it to dip music or game sound while you talk. Choose the key strip in the effect's panel. **Attack**, **Hold** and **Release** shape the dip.

The GR meter shows how far it's dipping. A strip can't be keyed from a strip it feeds, or from a strip keyed by it: that would be a loop, and Patch Prime refuses it.

### Broadcast leveler

Slowly turns the strip up or down so it sits at a steady loudness, like a radio station's processing.

- **Target**: the loudness to aim for, in LUFS. About −16 for streams and podcasts, −23 for broadcast.
- **Max boost** and **Max cut**: how far it may go.
- **Speed**: how many seconds a change takes.
- **Freeze below**: when the sound drops below this, it holds still, so silence and room noise aren't turned up.

The GAIN meter shows the gain it's applying, and **Loudness** what it measures.

### Safety limiter

Nothing leaves the strip above the **Ceiling** (−1 dB to start). It looks 1.5 ms ahead, so peaks are caught before they arrive instead of being clipped. **Release** sets how fast it recovers. The GR meter shows how much it's limiting. Put it last on a stream mix or an output.

### Reverb

An algorithmic reverb.

- **Size** and **Decay**: the room, and how long the tail lasts.
- **Pre-delay**: a gap before the reverb starts, which keeps a voice clear.
- **Damping**: how quickly the highs die away.
- **Width**: from mono to wide stereo.
- **Mix**: how much reverb you hear.

As an insert on one strip, keep the mix low. For several strips, use a reverb bus; see [Recipes](#recipes).

### Delay (line up outputs)

Delays the strip by up to one second. Use it on a fast output to line it up with a slower one, such as Bluetooth headphones or an HDMI TV.

### Gain

A plain level trim, ±24 dB.

## Meters

Each channel's meter shows three things:

- a **solid bar**: the RMS level, roughly how loud it sounds;
- a **lighter bar** above it: the peak level;
- a **thin line**: the highest recent peak. It holds for about a second and a half, then falls.

The colors turn yellow near full scale and red at the top.

Mix buses and outputs show their **loudness** under the meters: short-term LUFS over the last 3 seconds. Streams usually aim for about −14 to −16, podcasts −16, and broadcast −23.

The light by each strip's name glows whenever sound passes, even sound far too quiet for the meters. It's handy for checking that a route works.

### Effect meters

In a strip's effects window every effect shows its input and output levels (the same solid RMS and lighter peak bars, on a scale down to −80 dB) and, for dynamics, its gain reduction as an amber bar hanging from the top; thresholds and the limiter's ceiling are lines across the meters. See [Effects](#effects). The input and output levels are only measured while the effects window is open, so they cost nothing otherwise.

On the mixer, the insert slot of a gate, ducker, multiband compressor, limiter or leveler shows its gain reduction as a thin amber bar along the slot's bottom edge, so you can see an effect working without opening the window.

## Audio engine settings

On the **Devices** page, under **Audio engine**:

- **Clock**: what keeps time for all your audio.
  - **Patch Prime** (recommended): unplugging any interface doesn't interrupt the others.
  - **An interface's own clock**: for studio setups that want one hardware master. If that interface is unplugged, Patch Prime keeps time until it returns, with a brief glitch on the other interfaces as the clock changes hands.
- **Sample rate**: 48 kHz is the default and suits almost everything.
- **Block size**: **Automatic** lets PipeWire choose. Smaller blocks mean less latency and more CPU.

Each hardware device has a **settings button** (the sliders icon) for its own sample rate and scheduling. Changing these restarts WirePlumber, which interrupts audio for a few seconds.

## Unplugging and plugging in devices

- When a device is unplugged, its strip shows **Offline** and keeps its settings. When it comes back, even in another USB port, everything reconnects by itself.
- A notification tells you when a device has been gone for more than 10 seconds, and when it's back.
- If a device comes back only half working (present, but with no sound), Patch Prime re-applies its profile once or twice to recover it.

## Recipes

**Stream your mic and desktop sound, hear them in headphones**

1. Make a mix bus called "Stream Mix" and a virtual input called "Desktop".
2. Star **Desktop**, so apps play into it.
3. Send your mic and Desktop to Stream Mix, and Desktop to your headphones.
4. In OBS, record `patchprime.stream-mix`.

**Clean up a microphone**

On the mic's strip, add these effects in this order:

1. Noise suppression
2. Noise gate
3. EQ, with a low cut around 80 Hz
4. Multiband compressor, or a broadcast leveler
5. Safety limiter

**Music that dips while you talk**

Add a **Ducker** to the music or Desktop strip and set its **Key** to your mic.

**A shared reverb**

1. Make a mix bus called "Reverb".
2. Add **Reverb** to it with **Mix** at 100 %.
3. Send the Reverb bus to your outputs.
4. Send any strip to Reverb. Each strip's send level sets how much reverb it gets.

**Check your stream's loudness**

Watch the LUFS number under Stream Mix. Add a broadcast leveler and a safety limiter to Stream Mix to keep it steady.

## Troubleshooting

**The window says the engine isn't running.**
The engine is a systemd user service. Check and start it with:

```sh
systemctl --user status patchprimed
systemctl --user restart patchprimed
```

Its log:

```sh
journalctl --user -u patchprimed
```

**An app doesn't show my virtual devices by name.**
Apps that list devices through PulseAudio show node names such as `patchprime.stream-mix`. Restart the app after creating or renaming a device.

**An app ignores the route I chose.**
If the app picks its own device in its settings, that wins. Change it there, or set the app to "default".

**There's a short burst of noise when I unplug my master interface.**
That happens only when an interface is the clock. Use **Patch Prime** as the clock for glitch-free unplugging.

**I want everything Patch Prime made gone from PipeWire.**
Stop the engine and remove its buses:

```sh
systemctl --user stop patchprimed
patchprimed --remove-buses
```

Starting the engine again rebuilds everything from your settings.

**Where are my settings?**
In `~/.config/patchprime/`:

- `devices.json`: devices, labels, levels, sends and effects;
- `apps.json`: app routes;
- `scenes/`: one file per scene.

Back up that folder to keep your setup.
