
#include "shared.h"
#include "sms_ntsc.h"
#include "md_ntsc.h"
#include "system.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <time.h>
#include <stdint.h>

#define SOUND_SHARED_MEM_NAME "/genesis_sound"
#define SOUND_SAMPLES_SIZE 2048  // Number of samples per channel
#define SOUND_CHANNELS 2         // Stereo
#define SOUND_BUFFER_SIZE (SOUND_SAMPLES_SIZE * SOUND_CHANNELS)
#define SOUND_FREQUENCY 48000

#define PSG_CHANNEL_SHARED_MEM_NAME "/genesis_psg_channels"
#define YM2612_CHANNEL_SHARED_MEM_NAME "/genesis_ym2612_channels"
#define YM2413_CHANNEL_SHARED_MEM_NAME "/genesis_ym2413_channels"
#define CHANNEL_CONTROL_SIZE (16 * sizeof(int))

#define SHARED_MEM_NAME "/genesis_frame"
#define FRAME_SIZE (FRAME_WIDTH * FRAME_HEIGHT * 4)

#define INPUT_SHARED_MEM_NAME "/genesis_input"
#define INPUT_SIZE sizeof(uint16)

#define CONTROL_SHARED_MEM_NAME "/genesis_control"
#define CONTROL_SIZE sizeof(int)

#define FRAME_RATE 60
#define FRAME_TIME_NS (1000000000LL / FRAME_RATE)
#define FRAME_WIDTH 320
#define FRAME_HEIGHT 240
#define VIDEO_WIDTH 320
#define VIDEO_HEIGHT 240


#define usleep

long long last_frame_time = 0;

// Function prototypes
static int setup_input_shared_memory(void);
static int setup_shared_memory(void);
static int setup_psg_channel_shared_memory(void);
static int setup_ym2612_channel_shared_memory(void);
static int setup_ym2413_channel_shared_memory(void);
void input_refresh(void);  // Declare the input_refresh function
static void read_external_input(void);
static void output_frame_data(void);
static void output_sound_data(void);

// Global variables
static int running = 1;

static int shared_mem_fd;
static void *shared_mem_ptr;

static int input_shared_mem_fd;
static void *input_shared_mem_ptr;

static int control_shared_mem_fd;
static void *control_shared_mem_ptr;

static int psg_channel_shared_mem_fd;
static int ym2612_channel_shared_mem_fd;
static int ym2413_channel_shared_mem_fd;
int *psg_channel_shared_mem_ptr = NULL;
int *ym2612_channel_shared_mem_ptr = NULL;
int *ym2413_channel_shared_mem_ptr = NULL;

int joynum = 0;

int log_error   = 0;
int debug_on    = 0;
int turbo_mode  = 0;
int use_sound   = 1;

// Replace fread calls with this function
static int safe_fread(void *ptr, size_t size, size_t nmemb, FILE *stream) {
    size_t read = fread(ptr, size, nmemb, stream);
    if (read != nmemb) {
        fprintf(stderr, "fread failed: expected %zu items, got %zu\n", nmemb, read);
        return 0;
    }
    return 1;
}

/* sound */
typedef struct {
    int sample_count;                  // Number of samples written
    short samples[SOUND_BUFFER_SIZE];  // Audio samples (stereo)
} sound_shared_mem_t;

static int sound_shared_mem_fd = -1;
static sound_shared_mem_t *sound_shared_mem_ptr = NULL;


static uint8 brm_format[0x40] =
{
  0x5f,0x5f,0x5f,0x5f,0x5f,0x5f,0x5f,0x5f,0x5f,0x5f,0x5f,0x00,0x00,0x00,0x00,0x40,
  0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
  0x53,0x45,0x47,0x41,0x5f,0x43,0x44,0x5f,0x52,0x4f,0x4d,0x00,0x01,0x00,0x00,0x00,
  0x52,0x41,0x4d,0x5f,0x43,0x41,0x52,0x54,0x52,0x49,0x44,0x47,0x45,0x5f,0x5f,0x5f
};

static short soundframe[SOUND_SAMPLES_SIZE];

/* video */
md_ntsc_t *md_ntsc;
sms_ntsc_t *sms_ntsc;

static int headless_video_init()
{
    bitmap.width = 320;
    bitmap.height = 224;
    bitmap.pitch = bitmap.width * 2;  // 16 bits per pixel (RGB565)
    bitmap.data = malloc(bitmap.pitch * bitmap.height);
    if (!bitmap.data) {
        fprintf(stderr, "Failed to allocate frame buffer\n");
        return 0;
    }
    bitmap.viewport.w = bitmap.width;
    bitmap.viewport.h = bitmap.height;
    bitmap.viewport.x = 0;
    bitmap.viewport.y = 0;
    bitmap.viewport.changed = 3;

    printf("Initialized bitmap: %dx%d, pitch: %d\n", 
           bitmap.width, bitmap.height, bitmap.pitch);

    return 1;
}

void handle_control_commands() {
    int command;
    memcpy(&command, control_shared_mem_ptr, CONTROL_SIZE);
    if (command == 1) {
        // Save state
        FILE *f = fopen("game.gp0", "wb");
        if (f) {
            uint8 buf[STATE_SIZE];
            int len = state_save(buf);
            fwrite(&len, sizeof(int), 1, f);  // Write the length first
            fwrite(buf, len, 1, f);           // Then write the state data
            fclose(f);
            printf("State saved with size: %d bytes.\n", len);
        } else {
            perror("Failed to open save state file for writing");
        }
        // Clear command
        int zero_command = 0;
        memcpy(control_shared_mem_ptr, &zero_command, CONTROL_SIZE);
    }

    else if (command == 2) {
        // Load state
        FILE *f = fopen("game.gp0", "rb");
        if (f) {
            int len;
            size_t read_len = fread(&len, sizeof(int), 1, f);
            if (read_len == 1) {
                uint8 *buf = malloc(len);
                if (buf) {
                    size_t read = fread(buf, 1, len, f);
                    if (read == len) {
                        state_load(buf);
                        printf("State loaded with size: %d bytes.\n", len);
                    } else {
                        fprintf(stderr, "fread failed: expected %d bytes, got %zu\n", len, read);
                    }
                    free(buf);
                } else {
                    fprintf(stderr, "Failed to allocate memory for state buffer.\n");
                }
            } else {
                fprintf(stderr, "Failed to read state length.\n");
            }
            fclose(f);
        } else {
            perror("Failed to open save state file for reading");
        }
        // Clear command
        int zero_command = 0;
        memcpy(control_shared_mem_ptr, &zero_command, CONTROL_SIZE);
    }
    else if (command == 3) {
        // Reset emulator
        system_reset();
        printf("Emulator reset.\n");
        
        // Clear command
        int zero_command = 0;
        memcpy(control_shared_mem_ptr, &zero_command, CONTROL_SIZE);
    }
}

void headless_input_update(void)
{
    read_external_input();
}

static void output_frame_data() {
    uint8_t *src = (uint8_t*)bitmap.data;
    uint8_t *dst = (uint8_t*)shared_mem_ptr;
    int width = 320;
    int height = 224;
    int src_pitch = bitmap.pitch;
    int bytes_per_pixel = 2;

    for (int y = 0; y < height; y++) {
        for (int x = 0; x < width; x++) {
            int src_offset = (y * src_pitch) + (x * bytes_per_pixel);
            int dst_offset = (y * width + x) * 4;

            uint16_t pixel = *(uint16_t*)(src + src_offset);
            uint8_t r = (pixel >> 11) & 0x1F;
            uint8_t g = (pixel >> 5) & 0x3F;
            uint8_t b = pixel & 0x1F;

            r = (r << 3) | (r >> 2);
            g = (g << 2) | (g >> 4);
            b = (b << 3) | (b >> 2);

            dst[dst_offset]     = r;     // R
            dst[dst_offset + 1] = g;     // G
            dst[dst_offset + 2] = b;     // B
            dst[dst_offset + 3] = 0xFF;  // A
        }
    }
}

static void output_sound_data(void) {
    int num_samples = audio_update(soundframe);

    // Ensure we don't exceed the buffer size
    if (num_samples > SOUND_SAMPLES_SIZE) {
        num_samples = SOUND_SAMPLES_SIZE;
    }

    // Populate the shared memory structure
    sound_shared_mem_t sound_data;
    sound_data.sample_count = num_samples;

    // Copy the sound samples (stereo interleaved)
    memcpy(sound_data.samples, soundframe, num_samples * SOUND_CHANNELS * sizeof(short));

    // Write the sound data to shared memory
    if (sound_shared_mem_ptr) {
        memcpy(sound_shared_mem_ptr, &sound_data, sizeof(sound_shared_mem_t));
    }
}

void input_refresh(void)
{
    uint16 pad_data;
    memcpy(&pad_data, input_shared_mem_ptr, INPUT_SIZE);
    input.pad[0] = pad_data;  // Assuming we're only handling player 1 input
}

static int setup_sound_shared_memory(void) {
    // Open or create the shared memory segment
    sound_shared_mem_fd = shm_open(SOUND_SHARED_MEM_NAME, O_CREAT | O_RDWR, 0666);
    if (sound_shared_mem_fd == -1) {
        perror("shm_open for sound");
        return 0;
    }

    // Set the size of the shared memory segment
    if (ftruncate(sound_shared_mem_fd, sizeof(sound_shared_mem_t)) == -1) {
        perror("ftruncate for sound");
        return 0;
    }

    // Map the shared memory segment into the process's address space
    sound_shared_mem_ptr = mmap(0, sizeof(sound_shared_mem_t), PROT_WRITE, MAP_SHARED, sound_shared_mem_fd, 0);
    if (sound_shared_mem_ptr == MAP_FAILED) {
        perror("mmap for sound");
        return 0;
    }

    // Initialize the shared memory with zeroes
    memset(sound_shared_mem_ptr, 0, sizeof(sound_shared_mem_t));

    return 1;
}

static int setup_control_shared_memory() {
    control_shared_mem_fd = shm_open(CONTROL_SHARED_MEM_NAME, O_CREAT | O_RDWR, 0666);
    if (control_shared_mem_fd == -1) {
        perror("shm_open for control");
        return 0;
    }

    if (ftruncate(control_shared_mem_fd, CONTROL_SIZE) == -1) {
        perror("ftruncate for control");
        return 0;
    }

    control_shared_mem_ptr = mmap(0, CONTROL_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, control_shared_mem_fd, 0);
    if (control_shared_mem_ptr == MAP_FAILED) {
        perror("mmap for control");
        return 0;
    }

    // Initialize control to 0
    memset(control_shared_mem_ptr, 0, CONTROL_SIZE);

    return 1;
}

static int setup_input_shared_memory() {
  input_shared_mem_fd = shm_open(INPUT_SHARED_MEM_NAME, O_CREAT | O_RDWR, 0666);
  if (input_shared_mem_fd == -1) {
    perror("shm_open for input");
    return 0;
  }
  
  if (ftruncate(input_shared_mem_fd, INPUT_SIZE) == -1) {
    perror("ftruncate for input");
    return 0;
  }
  
  input_shared_mem_ptr = mmap(0, INPUT_SIZE, PROT_READ, MAP_SHARED, input_shared_mem_fd, 0);
  if (input_shared_mem_ptr == MAP_FAILED) {
    perror("mmap for input");
    return 0;
  }
  
  return 1;
}

static int setup_psg_channel_shared_memory(void) {
    psg_channel_shared_mem_fd = shm_open(PSG_CHANNEL_SHARED_MEM_NAME, O_CREAT | O_RDWR, 0666);
    if (psg_channel_shared_mem_fd == -1) {
        perror("shm_open for PSG channel control");
        return 0;
    }

    if (ftruncate(psg_channel_shared_mem_fd, CHANNEL_CONTROL_SIZE) == -1) {
        perror("ftruncate for PSG channel control");
        return 0;
    }

    psg_channel_shared_mem_ptr = mmap(0, CHANNEL_CONTROL_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, psg_channel_shared_mem_fd, 0);
    if (psg_channel_shared_mem_ptr == MAP_FAILED) {
        perror("mmap for PSG channel control");
        return 0;
    }

    // Initialize all channels to enabled (1)
    for (int i = 0; i < 16; i++) {
        psg_channel_shared_mem_ptr[i] = 1;
    }

    return 1;
}

static int setup_ym2612_channel_shared_memory(void) {
    ym2612_channel_shared_mem_fd = shm_open(YM2612_CHANNEL_SHARED_MEM_NAME, O_CREAT | O_RDWR, 0666);
    if (ym2612_channel_shared_mem_fd == -1) {
        perror("shm_open for YM2612 channel control");
        return 0;
    }

    if (ftruncate(ym2612_channel_shared_mem_fd, CHANNEL_CONTROL_SIZE) == -1) {
        perror("ftruncate for YM2612 channel control");
        return 0;
    }

    ym2612_channel_shared_mem_ptr = mmap(0, CHANNEL_CONTROL_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, ym2612_channel_shared_mem_fd, 0);
    if (ym2612_channel_shared_mem_ptr == MAP_FAILED) {
        perror("mmap for YM2612 channel control");
        return 0;
    }

    // Initialize all channels to enabled (1)
    for (int i = 0; i < 16; i++) {
        ym2612_channel_shared_mem_ptr[i] = 1;
    }

    return 1;
}

static int setup_ym2413_channel_shared_memory(void) {
    ym2413_channel_shared_mem_fd = shm_open(YM2413_CHANNEL_SHARED_MEM_NAME, O_CREAT | O_RDWR, 0666);
    if (ym2413_channel_shared_mem_fd == -1) {
        perror("shm_open for YM2413 channel control");
        return 0;
    }

    if (ftruncate(ym2413_channel_shared_mem_fd, CHANNEL_CONTROL_SIZE) == -1) {
        perror("ftruncate for YM2413 channel control");
        return 0;
    }

    ym2413_channel_shared_mem_ptr = mmap(0, CHANNEL_CONTROL_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, ym2413_channel_shared_mem_fd, 0);
    if (ym2413_channel_shared_mem_ptr == MAP_FAILED) {
        perror("mmap for YM2413 channel control");
        return 0;
    }

    // Initialize all channels to enabled (1)
    for (int i = 0; i < 16; i++) {
        ym2413_channel_shared_mem_ptr[i] = 1;
    }

    return 1;
}

static void read_external_input() {
    uint16 pad_data;
    memcpy(&pad_data, input_shared_mem_ptr, INPUT_SIZE);
    input.pad[0] = pad_data;  // Assuming we're only handling player 1 input
}

static int setup_shared_memory() {
  shared_mem_fd = shm_open(SHARED_MEM_NAME, O_CREAT | O_RDWR, 0666);
  if (shared_mem_fd == -1) {
    perror("shm_open");
    return 0;
  }
  
  if (ftruncate(shared_mem_fd, FRAME_SIZE) == -1) {
    perror("ftruncate");
    return 0;
  }
  
  shared_mem_ptr = mmap(0, FRAME_SIZE, PROT_WRITE, MAP_SHARED, shared_mem_fd, 0);
  if (shared_mem_ptr == MAP_FAILED) {
    perror("mmap");
    return 0;
  }
  
  return 1;
}

int main(int argc, char **argv)
{
  FILE *fp;
  int running = 1;
  /* Parse command-line arguments */
  if (argc < 1)
  {
    printf("Genesis Plus GX\nUsage: %s gamename\n", argv[0]);
    return 1;
  }

//   /* Check for headless flag */
//   if (strcmp(argv[1], "--headless") == 0)
//   {
//     headless = 1;
//     if (argc < 3)
//     {
//       printf("Genesis Plus GX\nUsage: %s --headless gamename\n", argv[0]);
//       return 1;
//     }
//   }

  /* set default config */
  error_init();
  set_config_defaults();

  /* mark all BIOS as unloaded */
  system_bios = 0;

  /* Genesis BOOT ROM support (2KB max) */
  memset(boot_rom, 0xFF, 0x800);
  fp = fopen(MD_BIOS, "rb");
  if (fp != NULL)
  {
    int i;

    /* read BOOT ROM */
    safe_fread(boot_rom, 1, 0x800, fp);
    fclose(fp);

    /* check BOOT ROM */
    if (!memcmp((char *)(boot_rom + 0x120),"GENESIS OS", 10))
    {
      /* mark Genesis BIOS as loaded */
      system_bios = SYSTEM_MD;
    }

    /* Byteswap ROM */
    for (i=0; i<0x800; i+=2)
    {
      uint8 temp = boot_rom[i];
      boot_rom[i] = boot_rom[i+1];
      boot_rom[i+1] = temp;
    }
  }

    if (!headless_video_init())
    {
        fprintf(stderr, "Headless video initialization failed\n");
        return 1;
    }
    if (!setup_shared_memory())
    {
        fprintf(stderr, "Shared memory setup failed\n");
        return 1;
    }
    if (!setup_input_shared_memory())
    {
        fprintf(stderr, "Input shared memory setup failed\n");
        return 1;
    }
    if (!setup_control_shared_memory())
    {
        fprintf(stderr, "Control shared memory setup failed\n");
        return 1;
    }
    if (!setup_sound_shared_memory())
    {
        fprintf(stderr, "Sound shared memory setup failed\n");
        return 1;
    }
    if (!setup_psg_channel_shared_memory())
    {
        fprintf(stderr, "PSG channel shared memory setup failed\n");
        return 1;
    }
    if (!setup_ym2612_channel_shared_memory())
    {
        fprintf(stderr, "YM2612 channel shared memory setup failed\n");
        return 1;
    }
    if (!setup_ym2413_channel_shared_memory())
    {
        fprintf(stderr, "YM2413 channel shared memory setup failed\n");
        return 1;
    }    
/* initialize Genesis virtual system */
  bitmap.viewport.changed = 3;

    if (!load_rom(argv[1]))
  {
      fprintf(stderr, "Error loading file '%s'\n", argv[1]);
      return 1;
  }

  /* initialize system hardware */
  audio_init(SOUND_FREQUENCY, 0);
  system_init();

  /* Mega CD specific */
  if (system_hw == SYSTEM_MCD)
  {
    /* load internal backup RAM */
    fp = fopen("./scd.brm", "rb");
    if (fp!=NULL)
    {
      safe_fread(scd.bram, 0x2000, 1, fp);
      fclose(fp);
    }

    /* check if internal backup RAM is formatted */
    if (memcmp(scd.bram + 0x2000 - 0x20, brm_format + 0x20, 0x20))
    {
      /* clear internal backup RAM */
      memset(scd.bram, 0x00, 0x200);

      /* Internal Backup RAM size fields */
      brm_format[0x10] = brm_format[0x12] = brm_format[0x14] = brm_format[0x16] = 0x00;
      brm_format[0x11] = brm_format[0x13] = brm_format[0x15] = brm_format[0x17] = (sizeof(scd.bram) / 64) - 3;

      /* format internal backup RAM */
      memcpy(scd.bram + 0x2000 - 0x40, brm_format, 0x40);
    }

    /* load cartridge backup RAM */
    if (scd.cartridge.id)
    {
      fp = fopen("./cart.brm", "rb");
      if (fp!=NULL)
      {
        safe_fread(scd.cartridge.area, scd.cartridge.mask + 1, 1, fp);
        fclose(fp);
      }

      /* check if cartridge backup RAM is formatted */
      if (memcmp(scd.cartridge.area + scd.cartridge.mask + 1 - 0x20, brm_format + 0x20, 0x20))
      {
        /* clear cartridge backup RAM */
        memset(scd.cartridge.area, 0x00, scd.cartridge.mask + 1);

        /* Cartridge Backup RAM size fields */
        brm_format[0x10] = brm_format[0x12] = brm_format[0x14] = brm_format[0x16] = (((scd.cartridge.mask + 1) / 64) - 3) >> 8;
        brm_format[0x11] = brm_format[0x13] = brm_format[0x15] = brm_format[0x17] = (((scd.cartridge.mask + 1) / 64) - 3) & 0xff;

        /* format cartridge backup RAM */
        memcpy(scd.cartridge.area + scd.cartridge.mask + 1 - sizeof(brm_format), brm_format, sizeof(brm_format));
      }
    }
  }

  if (sram.on)
  {
    /* load SRAM */
    fp = fopen("./game.srm", "rb");
    if (fp!=NULL)
    {
      safe_fread(sram.sram,0x10000,1, fp);
      fclose(fp);
    }
  }

  /* reset system hardware */
  system_reset();

  /* emulation loop */
  while (running)
  {
    long long current_time, target_time, sleep_time;

    // Get current time
    struct timespec current_spec;
    clock_gettime(CLOCK_MONOTONIC, &current_spec);
    current_time = current_spec.tv_sec * 1000000000LL + current_spec.tv_nsec;

    // Calculate target time for this frame
    target_time = last_frame_time + FRAME_TIME_NS;

    // Handle frame processing
    if (system_hw == SYSTEM_MCD) {
        system_frame_scd(0);
    } else if ((system_hw & SYSTEM_PBC) == SYSTEM_MD) {
        system_frame_gen(0);
    } else {
        system_frame_sms(0);
    }

    // Output frame and sound data
    output_frame_data();
    output_sound_data();

    // Handle control commands
    handle_control_commands();

    // Calculate sleep time
    sleep_time = target_time - current_time;

    if (sleep_time > 0) {
        struct timespec sleep_spec = {sleep_time / 1000000000LL, sleep_time % 1000000000LL};
        nanosleep(&sleep_spec, NULL);
    }

    // Update last frame time, accounting for potential overshoots
    last_frame_time = target_time;

    // If we've significantly overshot, reset timing
    if (current_time > target_time + FRAME_TIME_NS) {
        last_frame_time = current_time;
    }
  }

  if (sram.on)
  {
    /* save SRAM */
    fp = fopen("./game.srm", "wb");
    if (fp!=NULL)
    {
      fwrite(sram.sram,0x10000,1, fp);
      fclose(fp);
    }
  }

  if (input_shared_mem_ptr) {
    munmap(input_shared_mem_ptr, INPUT_SIZE);
    shm_unlink(INPUT_SHARED_MEM_NAME);
  }

  if (control_shared_mem_ptr) {
    munmap(control_shared_mem_ptr, CONTROL_SIZE);
    shm_unlink(CONTROL_SHARED_MEM_NAME);
  }

  if (sound_shared_mem_ptr) {
    munmap(sound_shared_mem_ptr, sizeof(sound_shared_mem_t));
    shm_unlink(SOUND_SHARED_MEM_NAME);
  }

  if (psg_channel_shared_mem_ptr) {
      munmap(psg_channel_shared_mem_ptr, CHANNEL_CONTROL_SIZE);
      shm_unlink(PSG_CHANNEL_SHARED_MEM_NAME);
  }
  if (ym2612_channel_shared_mem_ptr) {
      munmap(ym2612_channel_shared_mem_ptr, CHANNEL_CONTROL_SIZE);
      shm_unlink(YM2612_CHANNEL_SHARED_MEM_NAME);
  }
  if (ym2413_channel_shared_mem_ptr) {
      munmap(ym2413_channel_shared_mem_ptr, CHANNEL_CONTROL_SIZE);
      shm_unlink(YM2413_CHANNEL_SHARED_MEM_NAME);
  }

  audio_shutdown();
  error_shutdown();

  return 0;
}
