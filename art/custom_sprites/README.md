# Custom fighter sprites (tucked away)

These are the sprite images cut from the supplied artwork sheets, one folder per fighter, with a
`sprite.json` describing size and alignment. The game does not load them at the moment: the fighters
are drawn by code.

To bring them back, move the four folders (`kai`, `bronn`, `nyx`, `rhea`) into the `assets` folder.
The game picks up any fighter that has a folder there, and the release build packs `assets` into the `.exe`.
