// The home page's screenshots and video (web/public/media/), with the words
// that describe them. The editor's are OpenNova's own game; the hero and the
// retail shots are the original game's own data, drawn by OpenNova. A gallery tile shows
// its 800-wide thumbnail and links to the full 1600-wide image.

export interface Shot {
  src: string;
  thumb: string;
  alt: string;
  caption: string;
}

function shot(name: string, alt: string, caption: string): Shot {
  return { src: `/media/${name}.webp`, thumb: `/media/thumbs/${name}.webp`, alt, caption };
}

export const hero = {
  src: '/media/hero.webp',
  alt: "Joint Operations' training island seen from above, running in OpenNova",
};

// Retail Joint Operations, its own data drawn by OpenNova. The first is shown
// wide, at full size. (dfx2-green-hell-camp.webp waits for the site to name
// dfx2.exe beside Jointops.exe.)
export const retailShots: Shot[] = [
  {
    ...shot(
      'jo-blackhawk-dusk',
      'A Blackhawk lifting off its pad at dusk',
      'Joint Operations, Straits of Malacca: a Blackhawk lifts off at dusk.'
    ),
    thumb: '/media/jo-blackhawk-dusk.webp',
  },
  shot(
    'jo-river-patrol',
    'Riflemen patrolling the walkways of a stilt village',
    'Joint Operations, Operation Emerald Scorpion: AI riflemen on patrol.'
  ),
  shot('jo-hud', 'First-person view with the HUD, rifle and compass', "Joint Operations' own HUD, weapon and compass."),
];

export const editorShots: Shot[] = [
  shot(
    'editor-mission',
    'The editor showing a mission in 3D with patrol paths and labels',
    'A mission in 3D, with its paths and units.'
  ),
  shot(
    'editor-event-board',
    'The event board beside the mission view, an event read back as a sentence',
    'The event board: mission logic, read back as sentences.'
  ),
  shot('editor-terrain', 'Sculpting a hill with the terrain brush', 'Sculpting terrain with the brush.'),
];

export const timelapse = {
  poster: '/media/editor-timelapse-poster.webp',
  sources: [
    { src: '/media/editor-timelapse.webm', type: 'video/webm' },
    { src: '/media/editor-timelapse.mp4', type: 'video/mp4' },
  ],
  alt: 'Sped-up video: an outpost built on a hill in the OpenNova Editor, then played',
  caption:
    "Sped up (18 seconds): sculpting a hill, placing an outpost and adding an event in OpenNova's own base game, then playing it.",
};
