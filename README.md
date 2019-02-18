# YASP

## Introduction
Yet Another Speech Parser (YASP), is part of the ongoing effort to create an animated series (or film) using Blender 2.8. This project is being documented in www.openmovie.org.

YASP is a library which is built on [sphinxbase](https://github.com/cmusphinx/sphinxbase) and [pocketsphinx](https://github.com/cmusphinx/pocketsphinx). pockesphinx is an Automatic Speech Recognition (ASR) library, which is able to take wav clips or direct microphone input and convert the speech into text.

pocketsphinx's API gives the ability to gather metadata on the converted speech, termed hypothesis. This metadata includes the start and end time of each word, which is what's relevant in our case.

YASP works by taking in two inputs:
1. A wave file
2. A transcript file

Using these two inputs it generates a JSON file describing the start and duration of each word, as well as the start and duration of each phoneme in the word. Here is a sample of the JSON file.
```
{
    "word": "of",
    "start":        950,
    "duration":     17,
    "phonemes":     [{
          "phoneme":      "AH",
          "start":        950,
          "duration":     10
      }, {
          "phoneme":      "V",
          "start":        960,
          "duration":     8
      }]
}, {
    "word": "this",
    "start":        968,
    "duration":     18,
    "phonemes":     [{
        "phoneme":      "DH",
        "start":        968,
        "duration":     4
    }, {
        "phoneme":      "IH",
        "start":        972,
        "duration":     4
    }, {
        "phoneme":      "S",
        "start":        976,
        "duration":     11
    }]
}
```
## Use Case Scenario
I reckon the library can have many uses beyond the intended goal. For example, it can be used to get context on when a specific word was spoken. However, the primary usage I had in mind when building this library, is to be able to extract the timing information of speech and generate animation data for lip-syncing.

YASP will be used in a blender add-on, yet to be developed at the time of this writing, to generate animation data for the ManuelBasitionLab ([1](https://github.com/amirpavlo/manuelbastionilab)) ([2](https://github.com/animate1978/MB-Lab)) character generator add-on for blender.

## Operating System Support
YASP is only supported for linux, as this is my primary development environment. However, I don't think it would be hard to add support for other operating systems. Both basesphinx and pocketsphinx do support Windows and Mac. I have no intention to tackle this project anytime soon, however, contributions are always welcome.

## Setup, compile and run
A simple setup script, setup.sh, written in bash is provided to build the environment for YASP. It does the following

1. Clones basesphinx in YASP/basesphinx
2. Clones pocketsphinx in YASP/pocketsphinx
3. Builds basesphinx
4. Builds pocketsphinx

compile.sh is used to compile YASP.

run.sh is used to run YASP.

### Setup and Compile Example 
```
./setup.sh
./compile.sh
```
### Makefile Added
A makefile has been added so YASP can be compiled as follows:
```
./setup.sh
make
make package
```
"make package" create a yasp-package.tar.gz which includes all the bits needed to run yasp.
This tar.gz file can be untarred in any location and used

### Running Examples
#### With Transcript
Running YASP on a .wav file with a speech transcript. This is by far the most accurate way to get the timing breakdown of speech. Otherwise, the recognized hypothesis might not be an exact match to what was spoken.

```
./run -a </path/to/audiofile.wav> -t </path/to/transcript> -o </path/to/output.json>
```

#### Without Transcript
You can also just feed in the .wav file without specifying a transcript for the text. pocketsphinx does a generally good job of recognizing speech, but it's not always 100% accurate.
```
./run -a </path/to/audiofile.wav> -o </path/to/output.json>
```
Note: a transcript is generated automatically and put in the current dir. It's named "generated_hypothesis"

#### Without Transcript and Specify Path
You can specify the path to the generated transcript if you'd like to organize your output. This is an absolute path to the file to write the generated hypothesis to.
```
./run -a </path/to/audiofile.wave> -o </path/to/output.json> -g </path/to/generated_transcript.txt>
```
#### With python
Python 3.x is required. Currently run_python uses 3.7, but you can change that to the version installed on your machine. The run_python script simply sets the LD_LIBRARY_PATH properly.

```
export LD_LIBRARY_PATH=$PWD/sphinxinstall/lib/:$PWD/src
./run_python
>> import yasp
>> logs = yasp.yasp_logs()
>> yasp.yasp_setup_logging(logs, None, "TmpLogs")
>> str = yasp_interpret_get_str("/path/to/audiofile.wav", "/path/to/audiofile.txt", None)
>> print(str)
>> yas.yasp_finish_logging(logs)
```

## Sample Rate Limitation
.wav files need to be 16kHz or less. This limitation is inherit to pocketsphinx.

## Future Improvements
1. Take microphone input
2. Integrate with audio re-sampling library to remove the 16kHz limitation.
3. better transcript parser. Names are not recognized. How do we deal with it?
