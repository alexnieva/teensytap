
/*


  Teensy Tap

  A Teensy 3.2 interface that is designed to
  - capture finger taps from a FSR
  - communicate tap timings to a PC through USB
  - deliver auditory feedback, possibly delayed, at every tap
  - play a metronome sound.


  Floris van Vugt, Sept 2017
  Based on TapArduino


  This is code designed to work with Teensyduino
  (i.e. the Arduino IDE software used to generate code you can run on Teensy)

  Code modified in late 2020 in BRAMS by Alex Nieva to detect step synchronization.

 */


#include <SD.h>
#include <SPI.h>
#include <Audio.h>


// Load the samples that we will play for taps and metronome clicks, respectively
#include "AudioSampleTap.h"
#include "AudioSampleMetronome.h"
#include "AudioSampleEndsignal.h"

#include "DeviceID_custom.h"


/*
  Setting up infrastructure for capturing taps (from a connected FSR)
*/

boolean active = false; // Whether the tap capturing & metronome and all is currently active
boolean prev_active = false; // Whether we were active on the previous loop iteration

int fsrAnalogPin = 3; // FSR is connected to analog 3 (A3)
int fsrReading;      // the analog reading from the FSR resistor divider

// Definitions for the SD card
//File myFile;
const int chipSelect = 10;


// For interpreting taps
int tap_onset_threshold    = 600; // the FSR reading threshold necessary to flag a tap onset
int tap_offset_threshold   = 500; // the FSR reading threshold necessary to flag a tap offset
int min_tap_on_duration    = 200; // the minimum duration of a tap (in ms), this prevents double taps
int min_tap_off_duration   = 200; // the minimum time between offset of one tap and the onset of the next taps, again this prevents double taps


int tap_phase = 0; // The current tap phase, 0 = tap off (i.e. we are in the inter-tap-interval), 1 = tap on (we are within a tap)


unsigned long current_t            = 0; // the current time (in ms)
unsigned long prev_t               = 0; // the time stamp at the previous iteration (used to ensure correct loop time)
unsigned long next_event_embargo_t = 0; // the time when the next event is allowed to happen
unsigned long trial_end_t          = 0; // the time that this trial will end


unsigned long tap_onset_t = 0;  // the onset time of the current tap
unsigned long tap_offset_t = 0; // the offset time of the current tap
int           tap_max_force = 0; // the maximum force reading during the current tap
unsigned long tap_max_force_t = 0; // the time at which the maximum tap force was experienced




int missed_frames = 0; // ideally our script should read the FSR every millisecond. we use this variable to check whether it may have skipped a millisecond



int metronome_interval = 600; // Time between metronome clicks

unsigned long next_metronome_t            = 0; // the time at which we should play the next metronome beat
unsigned long next_feedback_t             = 0; // the time at which the next tap sound should occur


int metronome_clicks_played = 0; // how many metronome clicks we have played (used to keep track and quit)

boolean running_trial = false; // whether we are currently running the trial



/*
  Information pertaining to the trial we are currently running
*/

int auditory_feedback          = 0; // whether we present a tone at every tap
int auditory_feedback_delay    = 0; // the delay between the tap and the to-be-presented feedback
int metronome                  = 0; // whether to present a metronome sound
int metronome_nclicks_predelay = 0; // how many clicks of the metronome to occur before we switch on the delay (if any)
int metronome_nclicks          = 0; // how many clicks of the metronome to present on this trial
int ncontinuation_clicks       = 0; // how many continuation clicks to present after the metronome stops






/*
  Setting up the audio
*/

float sound_volume = .5; // the volume

// Create the Audio components.
// We create two sound memories so that we can play two sounds simultaneously
AudioPlayMemory    sound0;
AudioPlayMemory    sound1;
AudioMixer4        mix1;   // one four-channel mixer (we'll only use two channels)
AudioOutputI2S     headphones;

// Create Audio connections between the components
AudioConnection c1(sound0, 0, mix1, 0);
AudioConnection c2(sound1, 0, mix1, 1);
AudioConnection c3(mix1, 0, headphones, 0);
AudioConnection c4(mix1, 0, headphones, 1); // We connect mix1 to headphones twice so that the sound goes to both ears

// Create an object to control the audio shield.
AudioControlSGTL5000 audioShield;





/*
  Serial communication stuff
*/


int msg_number = 0; // keep track of how many messages we have sent over the serial interface (to be able to track down possible missing messages)

long baudrate = 9600; // the serial communication baudrate; not sure whether this actually does anything because Teensy documentation suggests that USB communication is always the highest possible.


const int MESSAGE_START   = 77;   // Signal to the Teensy to start
const int MESSAGE_CONFIG  = 88;   // Signal to the Teensy that we are going to send a trial configuration
const int MESSAGE_STOP    = 55;   // Signal to the Teensy to stop

const int CONFIG_LENGTH = 6*4; /* Defines the length of the configuration packet */
  








void setup(void) {
  /* This function will be executed once when we power up the Teensy */
  SPI.setMOSI(7);  // Audio shield has MOSI on pin 7
  SPI.setSCK(14);  // Audio shield has SCK on pin 14
  
  Serial.begin(baudrate);  // Initiate serial communication
  Serial.print("TeensyTap starting...\n");

  // Audio connections require memory to work.  For more
  // detailed information, see the MemoryAndCpuUsage example
  AudioMemory(10);

  // turn on the output
  audioShield.enable();
  audioShield.volume(sound_volume);

  // reduce the gain on mixer channels, so more than 1
  // sound can play simultaneously without clipping
  mix1.gain(0, 0.5);
  mix1.gain(1, 0.5);

  // SD card setup stuff
  Sd2Card card;
  SdVolume volume;
  File f1, f2, f3, f4;
  char buffer[512];
  boolean status;
  unsigned long usec, usecMax;
  elapsedMicros usecTotal, usecSingle;
  int i, type;
  float size;
//  Serial.print("Initializing SD card...");
//
//  if (!SD.begin(chipSelect)) {
//    Serial.println("initialization failed!");
//    return;
//  }
//  Serial.println("initialization done.");
  // First, detect the card
  status = card.init(SPI_FULL_SPEED, chipSelect);
  if (status) {
    Serial.println("SD card is connected :-)");
  } else {
    Serial.println("SD card is not connected or unusable :-(");
    return;
  }

  type = card.type();
  if (type == SD_CARD_TYPE_SD1 || type == SD_CARD_TYPE_SD2) {
    Serial.println("Card type is SD");
  } else if (type == SD_CARD_TYPE_SDHC) {
    Serial.println("Card type is SDHC");
  } else {
    Serial.println("Card is an unknown type (maybe SDXC?)");
  }

  // Then look at the file system and print its capacity
  status = volume.init(card);
  if (!status) {
    Serial.println("Unable to access the filesystem on this card. :-(");
    return;
  }

  size = volume.blocksPerCluster() * volume.clusterCount();
  size = size * (512.0 / 1e6); // convert blocks to millions of bytes
  Serial.print("File system space is ");
  Serial.print(size);
  Serial.println(" Mbytes.");

  // Now open the SD card normally
  status = SD.begin(chipSelect);
  if (status) {
    Serial.println("SD library is able to access the filesystem");
  } else {
    Serial.println("SD library can not access the filesystem!");
    Serial.println("Please report this problem, with the make & model of your SD card.");
    Serial.println("  http://forum.pjrc.com/forums/4-Suggestions-amp-Bug-Reports");
  }



  Serial.print("TeensyTap ready.\n");

  active = false;
}





void do_activity() {
  /* This is the usual activity loop */
  
  /* If this is our first loop ever, initialise the time points at which we should start taking action */
  if (prev_t == 0)           { prev_t = current_t; } // To prevent seeming "lost frames"
  if (next_metronome_t == 0) { next_metronome_t = current_t+metronome_interval; }
  
  if (current_t > prev_t) {
    // Main loop tick (one ms has passed)
    
    
    if ((prev_active) && (current_t-prev_t > 1)) {
      // We missed a frame (or more)
      missed_frames += (current_t-prev_t);
    }
    
    
    /*
     * Collect data
     */
    fsrReading = analogRead(fsrAnalogPin);
    
    
    

    /*
     * Process data: has a new tap onset or tap offset occurred?
     */

    if (tap_phase==0) {
      // Currently we are in the tap-off phase (nothing was thouching the FSR)
      
      /* First, check whether actually anything is allowed to happen.
   For example, if a tap just happened then we don't allow another event,
   for example we don't want taps to occur impossibly close (e.g. within a few milliseconds
   we can't realistically have a tap onset and offset).
   Second, check whether this a new tap onset
      */
      if ( (current_t > next_event_embargo_t) && (fsrReading>tap_onset_threshold)) {

  // New Tap Onset
  tap_phase = 1; // currently we are in the tap "ON" phase
  tap_onset_t = current_t;
  // don't allow an offset immediately; freeze the phase for a little while
  next_event_embargo_t = current_t + min_tap_on_duration;

  // Schedule the next tap feedback time (if we deliver feedback)
  if (metronome && metronome_clicks_played < metronome_nclicks_predelay) {
    next_feedback_t = current_t; // if we are in the pre-delay period, let's play the feedback sound immediately.
  } else {
    next_feedback_t = current_t + auditory_feedback_delay;
  }
      }
      
    } else if (tap_phase==1) {
      // Currently we are in the tap-on phase (the subject was touching the FSR)
      
      // Check whether the force we are currently reading is greater than the maximum force; if so, update the maximum
      if (fsrReading>tap_max_force) {
  tap_max_force_t = current_t;
  tap_max_force   = fsrReading;
      }
      
      // Check whether this may be a tap offset
      if ( (current_t > next_event_embargo_t) && (fsrReading<tap_offset_threshold)) {

  // New Tap Offset
  
  tap_phase = 0; // currently we are in the tap "OFF" phase
  tap_offset_t = current_t;
  
  // don't allow an offset immediately; freeze the phase for a little while
  next_event_embargo_t = current_t + min_tap_off_duration;

  // Send data to the computer!
  send_tap_to_serial();
  send_tap_to_sdCard();

  // Clear information about the tap so that we are ready for the next tap to occur
  tap_onset_t     = 0;
  tap_offset_t    = 0;
  tap_max_force   = 0;
  tap_max_force_t = 0;
  
      }
      
    }


    /* 
     * Deal with the metronome
    */
    // Is this a time to play a metronome click?
    if (metronome && (metronome_clicks_played < metronome_nclicks_predelay + metronome_nclicks)) {
      if (current_t >= next_metronome_t) {

  // Mark that we have another click played
  metronome_clicks_played += 1;

  // Play metronome click
  sound1.play(AudioSampleMetronome);
  
  // And schedule the next upcoming metronome click
  next_metronome_t += metronome_interval;
  
  // Proudly tell the world that we have played the metronome click
  send_metronome_to_serial();
  send_metronome_to_sdCard();
      }
    }

    if (auditory_feedback) {
      
      if ((next_feedback_t != 0) && (current_t >= next_feedback_t)) {

  // Play the auditory feedback (relating to the subject's tap)
  sound0.play(AudioSampleTap);

  // Clear the queue, nothing more to play
  next_feedback_t = 0;
  
  // Proudly tell the world that we have played the tap sound
  send_feedback_to_serial();
  send_feedback_to_sdCard();
      }

    }
    
    // Update the "previous" state of variables
    prev_t = current_t;
  }

}




void loop(void) {
  /* This is the main loop function which will be executed ad infinitum */

  current_t = millis(); // get current time (in ms)

  if (active) { do_activity(); }
  // Signal for the next loop iteration whether we were active previously.
  // For example, if we weren't active previously then we don't want to count lost frames.
  prev_active = active;


  if (active && running_trial && (current_t > trial_end_t)) {
    // Trial has ended (we have completed the number of metronome clicks and continuation clicks)

    // Play another sound to signal to the subject that the trial has ended.
    sound1.play(AudioSampleEndsignal);

    // Communicate to the computer
    Serial.print("# Trial completed at t=");
    Serial.print(current_t);
    Serial.print("\n");
    
    //Writing to the SD Card
    char msg[50] = "";
    sprintf(msg, "# Trial completed at t = %lu\n", current_t);
    write_to_sdCard(msg);
        
    running_trial = false;
  }


  
  /* 
     Read the serial port, see if some message is available for us.
  */
  if (Serial.available()) {
    int inByte = Serial.read();

    if (inByte==MESSAGE_CONFIG) { // We are going to receive config information from the PC
      read_config_from_serial();
    }
    
    if (inByte==MESSAGE_START) {  // Switch to active mode
      Serial.print("# Start signal received at t=");
      Serial.print(current_t);
      Serial.print("\n");

      //Writing to the SD Card
      char msg[50] = "";
      sprintf(msg, "# Start signal received at t = %lu\n", current_t);
      write_to_sdCard(msg);
    

      // Compute when this trial will end
      trial_end_t = current_t;
      if (metronome) {
  trial_end_t += (metronome_nclicks_predelay+metronome_nclicks+1)*metronome_interval; // the +1 here is because from the start moment we will wait one metronome period until we actually start registering
      }
      trial_end_t   += (ncontinuation_clicks*metronome_interval);
      
      active        = true;
      running_trial = true;

      next_feedback_t  = 0; // ensure that nothing is scheduled to happen any time soon
      next_metronome_t = 0;
      
      /* Okay, if we are playing a metronome then let's determine when to start. */
      if (metronome) {
  next_metronome_t = current_t + metronome_interval;
      }
    }
    
    if (inByte==MESSAGE_STOP) {   // Switch to inactive mode
      Serial.print("# Stop signal received at t=");
      Serial.print(current_t);
      Serial.print("\n");

      //Writing to the SD Card
      char msg[50] = "";
      sprintf(msg, "# Stop signal received at t = %lu\n", current_t);
      write_to_sdCard(msg);
    
      active = false; // Put our activity on hold
    }
    
  }

}





long readint() {
  /* Reads an int (well, really a long int in Arduino land) from the Serial interface */
  union {
    byte asBytes[4];
    long asLong;
  } reading;
  
  for (int i=0;i<4;i++){
    reading.asBytes[i] = (byte)Serial.read();
  }
  return reading.asLong;

}



void read_config_from_serial() {
  /* 
     This function runs when we are about to receive configuration
     instructions from the PC.
  */
  active = false; // Ensure we are not active while receiving configuration (this can have unpredictable results)
  Serial.print("# Receiving configuration...\n");

  while (!(Serial.available()>=CONFIG_LENGTH)) {
    // Wait until we have enough info
  }
  Serial.print("# ... starting to read...\n");
  
  auditory_feedback          = readint();
  auditory_feedback_delay    = readint();
  metronome                  = readint();
  metronome_interval         = readint();
  metronome_nclicks_predelay = readint();
  metronome_nclicks          = readint();
  ncontinuation_clicks       = readint();

  Serial.print("# Config received...\n");
  send_config_to_serial();
  send_config_to_sdCard();
  send_header();
  send_header_to_sdCard();

  // Reset some of the other configuration parameters
  missed_frames           = 0;
  metronome_clicks_played = 0;
  msg_number              = 0; // start messages from zero again
  
}




void send_config_to_serial() {
  /* Sends a dump of the current config to the serial. */

  char msg[200];
  //msg_number += 1; // This is the next message
  Serial.print  ("# Device installed ");
  Serial.println(DEVICE_ID);
  sprintf(msg, "# config AF=%i DELAY=%i METR=%i INTVL=%i NCLICK_PREDELAY=%i NCLICK=%i NCONT=%i\n",
    auditory_feedback,
    auditory_feedback_delay,
    metronome,
    metronome_interval,
    metronome_nclicks_predelay,
    metronome_nclicks,
    ncontinuation_clicks);
  Serial.print(msg);

}


void send_config_to_sdCard() {
  /* Sends a dump of the current config to the SD card*/
  char msg[200];
  //msg_number += 1; // This is the next message
//  Serial.print  ("# Device installed ");
//  Serial.println(DEVICE_ID);
  sprintf(msg, "# config AF=%i DELAY=%i METR=%i INTVL=%i NCLICK_PREDELAY=%i NCLICK=%i NCONT=%i\n",
    auditory_feedback,
    auditory_feedback_delay,
    metronome,
    metronome_interval,
    metronome_nclicks_predelay,
    metronome_nclicks,
    ncontinuation_clicks);
    write_to_sdCard(msg);
}


void send_header() {
  /* Sends information about the current tap to the PC through the serial interface */
  Serial.print("message_number type onset_t offset_t max_force_t max_force n_missed_frames\n");
}

void send_header_to_sdCard(){
  char msg[200] = "message_number type onset_t offset_t max_force_t max_force n_missed_frames\n";
  write_to_sdCard(msg);
}

void write_to_sdCard(char *msg){
  // open the file.
  File dataFile = SD.open("datalog.txt", FILE_WRITE);

  // if the file is available, write to it:
  if (dataFile) {
    dataFile.println(msg);
    dataFile.close();
    // print to the serial port too:
    //Serial.println(dataString);
  }  
  // if the file isn't open, pop up an error:
  else {
    Serial.println("error opening datalog.txt");
  }  
}


void send_tap_to_serial() {
  /* Sends information about the current tap to the PC through the serial interface */
  char msg[100];
  msg_number += 1; // This is the next message
  sprintf(msg, "%d tap %lu %lu %lu %d %d\n",
    msg_number,
    tap_onset_t,
    tap_offset_t,
    tap_max_force_t,
    tap_max_force,
    missed_frames);
  Serial.print(msg);
  
}

void send_tap_to_sdCard(){
  /* Sends information about the current tap to the SD card  */
  char msg[100];
  msg_number += 1; // This is the next message
  sprintf(msg, "%d tap %lu %lu %lu %d %d\n",
    msg_number,
    tap_onset_t,
    tap_offset_t,
    tap_max_force_t,
    tap_max_force,
    missed_frames);
  write_to_sdCard(msg);  
}




void send_feedback_to_serial() {
  /* Sends information about the current tap to the PC through the serial interface */
  char msg[100];
  msg_number += 1; // This is the next message
  sprintf(msg, "%d feedback %lu NA NA NA %d\n",
    msg_number,
    current_t,
    missed_frames);
  Serial.print(msg);
}

void send_feedback_to_sdCard() {
  /* Sends information about the current tap to the SD card */
  char msg[100];
  msg_number += 1; // This is the next message
  sprintf(msg, "%d feedback %lu NA NA NA %d\n",
    msg_number,
    current_t,
    missed_frames);
  write_to_sdCard(msg);
}

void send_metronome_to_serial() {
  /* Sends information about the current tap to the PC through the serial interface */
  char msg[100];
  msg_number += 1; // This is the next message
  sprintf(msg, "%d click %lu NA NA NA %d\n",
    msg_number,
    current_t,
    missed_frames);
  Serial.print(msg);
}

void send_metronome_to_sdCard() {
  /* Sends information about the current tap to the SD card */
  char msg[100];
  msg_number += 1; // This is the next message
  sprintf(msg, "%d click %lu NA NA NA %d\n",
    msg_number,
    current_t,
    missed_frames);
  write_to_sdCard(msg);
}
