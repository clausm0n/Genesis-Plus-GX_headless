# Genesis Plus GX Headless

This is a fork of the Genesis Plus GX emulator, modified to run as a headless console application that exposes shared memory for external programs to access. This version allows for easy integration with other applications, making it suitable for various use cases such as game analysis, AI training, or custom frontend development.

## Features

- Headless operation (no GUI)
- Shared memory interfaces for video, audio, and input
- Support for Sega Genesis/Mega Drive, Sega CD, and Master System
- High compatibility and accuracy

## Building

To build the emulator, you'll need GCC and standard development libraries. Follow these steps:

1. Clone this repository
2. Navigate to the project directory
3. Run `make -f MakeFile.headless`

The build process will create an executable named `gx_headless` in the project directory.

## Usage

Run the emulator from the command line:

```
./gx_headless <path_to_rom>
```
## Shared Memory Interfaces

The emulator exposes several shared memory segments for external applications to interact with:

1. Video Frame Data: `/genesis_frame`
   - Size: 320x240x4 bytes (RGBA format)
   - Updated every frame

2. Audio Data: `/genesis_sound`
   - Structure: `sound_shared_mem_t`
   - Contains latest audio samples

3. Input Data: `/genesis_input`
   - Size: 2 bytes (16-bit integer)
   - Write to this to send input to the emulator

4. Control Commands: `/genesis_control`
   - Size: 4 bytes (32-bit integer)
   - Used for save states, loading states, and reset commands

5. Audio Channel Control:
   - PSG Channels: `/genesis_psg_channels`
   - YM2612 Channels: `/genesis_ym2612_channels`
   - YM2413 Channels: `/genesis_ym2413_channels`
   - Each is 64 bytes (16 int32 values)

## Key Functions

- `setup_shared_memory()`: Initializes shared memory for video frames
- `setup_sound_shared_memory()`: Sets up shared memory for audio data
- `setup_input_shared_memory()`: Creates shared memory for input data
- `setup_control_shared_memory()`: Establishes shared memory for control commands
- `output_frame_data()`: Writes the current frame to shared memory
- `output_sound_data()`: Outputs audio samples to shared memory
- `handle_control_commands()`: Processes control commands (save/load state, reset)

## External Integration

To integrate with this emulator:

1. Open the shared memory segments in your application
2. Read frame data and audio samples as needed
3. Write input data to control the emulator
4. Use control commands for save states and resets

## License

This project is licensed under the same terms as the original Genesis Plus GX emulator. Please refer to the original project for more details. 

## Acknowledgments

This fork is based on the excellent work of the Genesis Plus GX team. All credit for the core emulation goes to them.