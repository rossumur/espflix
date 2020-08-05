# ESPFLIX: A free video streaming service that runs on a ESP32


**ESPFLIX** is designed to run on the ESP32 within the Arduino IDE framework. See it in action on [Youtube](https://www.youtube.com/watch?v=qFRkfeuTUrU). Like the [ESP_8_BIT](https://rossumblog.com/2020/05/10/130/), the schematic is pretty simple:

```
    -----------
    |         |
    |      25 |------------------> video out
    |         |
    |      18 |---/\/\/\/----|---> audio out
    |         |     1k       |
    |         |             ---
    |  ESP32  |             --- 10nf
    |         |              |
    |         |              v gnd
    |         |
    |         |     3.3v <--+-+   IR Receiver
    |         |      gnd <--|  )  TSOP4838 etc.
    |       0 |-------------+-+       -> AppleTV remote control
    -----------

```

# How It Works
Building on the NTSC/PAL software video output created for [ESP_8_BIT](https://rossumblog.com/2020/05/10/130/), ESPFLIX adds video and audio codecs and a AWS streaming service to deliver a open source pastiche of Netflix.

### MPEG1 Software Video Codec
In 1993, [Compact Disc Digital Video](https://en.wikipedia.org/wiki/Video_CD) was introduced. Using the MPEG1 video codec, it queezed 74 minutes onto a CD. Earlier that year the Voyager Company had used Quicktime and the [Cinepak](https://en.wikipedia.org/wiki/Cinepak) software codec to produce first movie ever to be relased on CD-ROM: [The Beatle's *Hard Days Night.*](https://www.nytimes.com/1993/04/13/science/personal-computers-at-last-a-movie-fits-on-a-cd-rom-disk.html)

While codecs have improved in the intervening decades the MPEG1 codec uses many of the same techniques as modern codecs: transform coding, motion estimation and variable length coding of prediction residuals are still the foundations of modern codecs like H264.

The standard MPEG1 resolution of 352x240 (NTSC) or 352x288 (PAL) seems like a good match for our ESP32 video output resolution. Because MPEG1 can encode diffences between frames (predicted or "P" frames) you need 2 frame buffers at this resoultion. MPEG1 normally also encodes differences between past and *future* frames (bidirectionally predicted or "B" frames) so that means 3 buffers.

A quick bit of math on the size of these frame buffers (each encoded in YUV 4:1:1 color space) yields ```352 * 288 * 3 * 1.5 = 456192``` which much more memory than the ESP32 has to offer. So we need to make a few concessions. We can live without B frames: they improve overall coding performance but it is easy to create nice looking video without them. We can also reduce the vertical resoultion: 1993 was still a 4:3 world, 352x192 is a fine resolution for the 2020s.

Even though ```352 * 192 * 2 * 1.5 = 202752``` seems a lot more managable getting a MPEG1 software codec be performant still has its challenges. You can't just ```malloc``` contiguous buffers of that size in one piece on an ESP32. They need to be broken up into strips and all the guts of the codec that does fiddly half-pixel motion compensation has to be adjusted according. Much of this memory needs to be allocated from a pool that can only be addressed 32 bits at a time, further complicating code that needs to run as past as possible. If the implemetation of the MPEG1 decoder looks weird it is normally because it is trying to deal with aligned access and chunked allocation of the frame buffers.

### SBC Software Audio Codec
SBC is a low-complexity subband codec specified by the Bluetooth Special Interest Group (SIG) for the Advanced Audio Distribution Profile (A2DP). Unlike the MP2 codec typically used along side MPEG1 video, SBC uses tiny 128 sample buffers (as opposed to 1152 for MP2). That may not sound like much but with so little memory available it made the world of difference.

I originally wrote this implementation for a Cortex M0: it works fine on tha tiny device with limited memory and no hardware divider. Its low complexity is handy given the SBC audio codec runs on core 1 of the ESP32 alongside the video NTSC/PAL encoding, the IP stack and the Delta-Sigma modulator.

### Delta-Sigma (ΔΣ; or Sigma-Delta, ΣΔ) Modulators
I love Delta-Sigma modulators. Despite the vigorous debate over the [correct name ordinality](https://www.laphamsquarterly.org/rivalry-feud/crack) they are an versatile tool that can be used for both high performance [ADCs](https://hackaday.com/2016/07/07/tearing-into-delta-sigma-adcs-part-1/) and [DACs](https://en.wikipedia.org/wiki/Direct_Stream_Digital). A great introduction can be found at https://www.beis.de/Elektronik/DeltaSigma/DeltaSigma.html.

The ESP32 has one built into I2S0. Sadly we are using I2S0 for video generation, so we will have to generate our signal in software. To turn a 48khz, 16 bit mono PCM stream into oversampled single bit stream we will have to choose a modulator that has nice noise characteristics but is fast enough to run on already busy microcontroller.

For this design I settled on a second order cascade-of-resonators, feedback form (CRFB) delta sigma modulator running at a 32x oversample rate. 32x is normally lower than one would like (64x is more typical) but given the already tight constraints on buffering and compute it seemed like a fair tradeoff. The noise shaping is good enough to shove the quantization energy higher in the spectrum allowing the RC filter on the audio pin to do its job efficiently. For each 16 bit PCM sample we produce 32 bits that we jam into the I2S peripheral as if it were a 48khz stereo PCM stream. The resulting 1.5Mbit modulated signal sounds pretty nice once filtered.

I used the great tool hosted at the University of Ulm to design the filter and calculate coefficients to get the desired noise shaping characteristics: https://www.sigma-delta.de/  

![Delta Sigma Design](img/ds.png)  
*Fig.1 CRFB 2nd Order Delta Sigma Modulator*

![Frequency Response](img/ds_simulation.png)  
*Fig.2 Noise shaping pushes quatization errors to higher frequencies*

## Display
### Scrolling & Animation
## Streaming
### Trickmodes and Indexing
## WiFi GUI

# New this month on EspFlix
![Posters of Lineup](img/lineup.jpg)
Ok. So it is a slightly smaller collection than Netflix but still stuff that is funny/enjoyable/interesting. Big shout out to the brilliant Nina Paley for all her great work.

The ESP32 is a great little device. Kind of amazing you can create a settop box for less than the price of a remote control and that services like AWS enable a streaming service like this for very little time/money.

cheers,
Rossum
