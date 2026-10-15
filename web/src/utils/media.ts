// The home page's screenshots and video (web/public/media/), with the words
// that describe them. Retail shots are the original games' own data, drawn by
// OpenNova; the editor's are OpenNova's own base game.

export interface Shot {
  src: string;
  alt: string;
  caption: string;
}

export const hero: Shot = {
  src: '/media/hero.webp',
  alt: "Joint Operations' training island seen from above, running in OpenNova",
  caption: '',
};

export const gameShots: Shot[] = [
  {
    src: '/media/jo-blackhawk-dusk.webp',
    alt: 'A Blackhawk lifting off its pad at dusk',
    caption: 'Joint Operations, Straits of Malacca: a Blackhawk lifts off at dusk.',
  },
  {
    src: '/media/jo-river-patrol.webp',
    alt: 'Riflemen patrolling the walkways of a stilt village',
    caption: 'Joint Operations, Operation Emerald Scorpion: AI riflemen patrol a stilt village.',
  },
  {
    src: '/media/jo-hud.webp',
    alt: 'First-person view with the HUD, rifle and compass',
    caption: "Joint Operations' own HUD, weapon and compass.",
  },
  {
    src: '/media/jo-monsoon.webp',
    alt: 'Rain and overcast over a stilt village',
    caption: 'Joint Operations: rain over the Emerald Scorpion village.',
  },
  {
    src: '/media/jo-escalation-dusk.webp',
    alt: 'A hazy sunset over the jungle',
    caption: 'Joint Operations: Escalation, Klong River Slums at sunset.',
  },
  {
    src: '/media/dfx2-green-hell-camp.webp',
    alt: 'A jungle camp below a mountain',
    caption: 'Delta Force Xtreme 2, Green Hell: a camp below the mountain.',
  },
  {
    src: '/media/dfx2-green-hell-jungle.webp',
    alt: 'Morning sun over a palm jungle',
    caption: 'Delta Force Xtreme 2, Green Hell: morning over the jungle.',
  },
];

export const editorShots: Shot[] = [
  {
    src: '/media/editor-mission.webp',
    alt: 'The editor showing a mission in 3D with patrol paths and labels',
    caption: 'A mission in 3D, with its paths and units.',
  },
  {
    src: '/media/editor-event-board.webp',
    alt: "The event board beside the mission view, an event read back as a sentence",
    caption: 'The event board: mission logic, read back as sentences.',
  },
  {
    src: '/media/editor-terrain.webp',
    alt: 'Sculpting a hill with the terrain brush',
    caption: 'Sculpting terrain with the brush.',
  },
  {
    src: '/media/editor-model-viewer.webp',
    alt: 'The model viewer showing a mud-brick hut',
    caption: 'The model viewer, with the files that use the model.',
  },
  {
    src: '/media/editor-map.webp',
    alt: "The island's 2D map beside the mission view",
    caption: "The mission's 2D map.",
  },
  {
    src: '/media/editor-weapon-definition.webp',
    alt: "A rifle's weapon.def row drawn as the game draws it, with its fields",
    caption: 'A weapon definition, drawn as the game draws it.',
  },
];

export const timelapse = {
  poster: '/media/editor-timelapse-poster.webp',
  sources: [
    { src: '/media/editor-timelapse.webm', type: 'video/webm' },
    { src: '/media/editor-timelapse.mp4', type: 'video/mp4' },
  ],
  alt: 'Sped-up video: an outpost built on a hill in the OpenNova Editor, then played',
  caption:
    "Sped up: sculpting a hill, placing an outpost and adding an event in OpenNova's own base game, then playing it.",
};
