import { $, el } from '../core/dom.js';

// 3. An ACE conversation (recorded from the iPod's assistantd)
export default (fig) => {
  const steps = [
    ['c', 'ACE /ace HTTP/1.0 · AA CC EE 02', 'Siri opens the connection with an HTTP-looking request line, then the stream magic. From here on, each direction is one zlib stream.'],
    ['s', 'HTTP/1.1 200 OK · AA CC EE 02', 'acesrv answers in kind.'],
    ['c', 'LoadAssistant', 'Siri asks for its account by id. The first time, it sends CreateAssistant instead, and saves the ids acesrv hands back.'],
    ['s', 'AssistantLoaded', 'No authentication: assistantd skips it because the preferences say "Authentication Disabled".'],
    ['c', 'SetRestrictions · ClearContext', 'Context the real server would use. acesrv needs no reply for these.'],
    ['c', 'GetAnchors', 'Siri wants to sync contacts, apps and so on, and won\'t send any speech until this is answered.'],
    ['s', 'GetAnchorsResponse (no anchors)', 'acesrv keeps no synced data, so it reports none.'],
    ['c', 'StartSpeechRequest  Speex_WB_Quality8', 'Siri heard you start talking. acesrv tells localsirid to read the speech model into RAM now, while you are still speaking.'],
    ['c', 'SpeechPacket × 12', 'Your voice: PCM or Speex, whatever the label says. acesrv tells them apart by data rate.'],
    ['c', 'Chunk', 'A sync chunk arrives in the middle of the speech.'],
    ['s', 'ChunkAccepted', 'acesrv accepts every chunk, echoing its new generation.'],
    ['c', 'FinishSpeech', 'The tweak\'s end-of-speech detection decided you stopped. Audio goes to localsirid on a worker thread; the main loop keeps answering pings.'],
    ['s', 'SpeechRecognized  "What is the capital of France?"', 'Sent as soon as speech recognition finishes, about 3 s later, so your words appear while the language model works.'],
    ['s', 'AddViews  AssistantUtteranceView  "The capital of France is Paris."', 'The answer, with speakableText, so Siri also reads it aloud.'],
    ['s', 'RequestCompleted', 'Done. Siri plays its end tone and speaks.'],
    ['c', 'Metrics', 'Siri reports timing data to the server, which acesrv ignores.'],
  ];
  const box = $(fig, '.sx-ace'), note = $(fig, '.sx-note');
  box.append(el('div', 'sx-hdr left', 'Siri (assistantd)'), el('div', 'sx-hdr', ''), el('div', 'sx-hdr right', 'acesrv'));
  const rows = steps.map(([who, text]) => {
    const m = el('div', 'msg ' + (who === 'c' ? 'left' : 'right'), text);
    const a = el('div', 'arrow', who === 'c' ? '→' : '←');
    if (who === 'c') box.append(m, a, el('div')); else box.append(el('div'), a, m);
    return [m, a];
  });
  let n = 0;
  const show = (k) => {
    n = k;
    rows.forEach(([m, a], i) => { m.classList.toggle('shown', i < n); a.classList.toggle('shown', i < n); m.classList.toggle('current', i === n - 1); });
    note.textContent = n ? steps[n - 1][2] : 'Pings and pongs flow in both directions throughout; they are left out here.';
  };
  fig.querySelectorAll('button').forEach((b) => b.addEventListener('click', () => {
    if (b.dataset.act === 'next') show(n >= steps.length ? 0 : n + 1);
    if (b.dataset.act === 'all') show(steps.length);
    if (b.dataset.act === 'reset') show(0);
  }));
  show(0);
};
