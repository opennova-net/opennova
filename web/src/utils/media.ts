// The public pages' screenshots and video (web/public/media/), with the words
// that describe them. The Acid Gambit shot is OpenNova's own game in its
// editor; the hero and the retail shots are the original games' own data,
// drawn by OpenNova.

export interface Shot {
  // The image a card shows (an 800-wide thumbnail, or the full image).
  src: string;
  alt: string;
}

export const hero = {
  src: '/media/hero.webp',
  alt: "Joint Operations' training island seen from above, running in OpenNova",
};

// Each game's card picture, by the Play page's game id.
export const gameShots: Record<'opennova' | 'jointops' | 'dfx2', Shot> = {
  opennova: {
    src: '/media/thumbs/editor-mission.webp',
    alt: 'The editor showing a mission in 3D with patrol paths and labels',
  },
  jointops: { src: '/media/jo-blackhawk-dusk.webp', alt: 'A Blackhawk lifting off its pad at dusk' },
  dfx2: { src: '/media/thumbs/dfx2-green-hell-camp.webp', alt: 'A jungle camp in Delta Force Xtreme 2' },
};

export const editorShowcase = {
  poster: '/media/editor-showcase-poster.webp',
  sources: [
    { src: '/media/editor-showcase.webm', type: 'video/webm' },
    { src: '/media/editor-showcase.mp4', type: 'video/mp4' },
  ],
  alt: 'OpenNova Editor showcase: terrain, mission building, patrols, events, animation and menu previews, then gameplay',
  description: 'In 30 seconds: shape terrain, place an outpost, give units patrol routes, create mission logic, preview an animated model, arrange a menu, and play the mission.',
};
