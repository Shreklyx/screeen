'use strict';

// Sharing the sound where the browser has no MediaStreamTrackProcessor (Firefox, Safari): the
// samples of each render quantum (128 per channel) go to the page as they come, which makes them
// AudioData for the Opus encoder.

class CaptureAudio extends AudioWorkletProcessor {
  process(inputs) {
    const input = inputs[0];
    if (input?.length) {
      const planes = input.map((channel) => channel.slice());
      this.port.postMessage({ planes }, planes.map((p) => p.buffer));
    }
    return true;
  }
}

registerProcessor('capture-audio', CaptureAudio);
