import { $, el, press } from '../core/dom.js';

// 1. Where each piece of Siri comes from
export default (fig) => {
  const items = [
    ['ipod', 'AssistantServices.framework', 'binary in the iPod\'s cache', 'Already in the iPod\'s dyld shared cache: the client side of Siri that SpringBoard and Settings use to talk to assistantd. Only its resource files (Info.plist, strings) are missing.'],
    ['ipod', 'SAObjects.framework', 'binary in the iPod\'s cache', 'The Objective-C classes for every ACE object (SASpeechRecognized, SAUIAddViews, ...). Also already in the iPod\'s cache.'],
    ['ipod', 'VoiceServices / voiced', 'speech synthesis', 'Siri speaks with the same text-to-speech engine VoiceOver uses, which every iPod has.'],
    ['ipod', 'AVVoiceController', 'recording', 'Records your voice and decides when you have stopped talking. Present, but tuned for the 4S: see section 5.'],
    ['ipod', 'MobileGestalt', 'says "no Siri here"', 'The capability database. It answers false for the "assistant" key, so the tweak hooks it (section 4).'],
    ['file', 'assistantd', 'the Siri daemon', 'A normal executable in AssistantServices.framework on the 4S. It records audio, keeps the ACE session with the server, and relays results to SpringBoard. All 35 libraries it links are on the iPod.'],
    ['file', 'Assistant.uibundle', 'SpringBoard plugin', 'The Siri screen itself: the paper-texture bubbles, the glowing microphone. A loadable bundle, not in the cache.'],
    ['file', '/System/Library/Assistant', 'UI plugins', 'Snippet plugins for maps, weather, stocks, reminders and so on. They load, but this project never sends their objects.'],
    ['file', 'Settings, LaunchDaemons', 'and resources', 'Siri\'s page in Settings, the launchd jobs for assistantd and assistant_service, and the frameworks\' resource folders: about 45 MB in all.'],
    ['cache', 'AssistantUI.framework', '137 KB, repaired', 'The only binary that exists solely inside the 4S\'s shared cache. Extracting it and making it load elsewhere is section 3.'],
    ['ours', 'SiriCapability tweak', 'Substrate', 'Says yes to the capability check, makes recording work without a Speex encoder, replaces the end-of-speech detection, and disables timeouts sized for Apple\'s servers.'],
    ['ours', 'acesrv', 'ACE server', 'Stands in for guzzoni.apple.com on 127.0.0.1:4443 (section 6).'],
    ['ours', 'localsirid', 'speech + LLM', 'Whistle and SmolLM2-135M in C and NEON (section 8).'],
    ['ours', '/etc/hosts entry', 'guzzoni.apple.com → 127.0.0.1', 'Makes sure Siri can only ever reach the iPod itself (section 7).'],
  ];
  const grid = $(fig, '.sx-inv'), note = $(fig, '.sx-note');
  const buttons = items.map(([src, name, sub, text]) => {
    const b = el('button', 'sx-item'); b.dataset.src = src; b.type = 'button';
    b.append(name, el('small', null, sub));
    b.addEventListener('click', () => { press(buttons, b); note.textContent = text; });
    grid.append(b); return b;
  });
};
