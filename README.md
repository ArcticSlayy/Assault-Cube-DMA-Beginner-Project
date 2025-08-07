## DMA Cheat Base

## Table of Contents
- [DMA Cheat Base](#dma-cheat-base)
- [Table of Contents](#table-of-contents)
- [Introduction](#introduction)
- [Todo](#todo)
- [Usage](#usage)
- [Credits](#credits)

## Introduction
This is a simple base to start building your Direct Memory Access (DMA) cheats.

I've currently implemented a basic cheat that reads player positions and displays them on the screen with boxes and names for now for Assault Cube. This is a beginner project so it's not optimized well, and has ESP stuttering. 

## Todo
- [X] Fix the nullptr errors when hitting Unload in the Info tab.
- [ ] Fix the ESP giving some weird hitch effect when rendering the visuals around players.
- [X] Re-implement the Visuals to toggle ESP on and off, it's currently always on for debugging purposes.
- [ ] Add more features to the cheat, like aimbot, triggerbot, etc.
- [X] Make the Weapon ESP more accurate, currently it just says the weapon name is a Knife which is not true.
## Usage
1. Clone the repository.
2. Open the project in Visual Studio.
3. Build the DMALibrary, and update it when it updates.
4. Configure `Config Structs` with your proccess info & kmbox details.
5. Add your own cheat code & initialize it.
6. Build the project & send it over to your second machine.
7. Magic!

## Credits
- [Ulf Frisk](https://github.com/ufrisk) for [MemProcFS](https://github.com/ufrisk/MemProcFS)
- [Metick](https://github.com/Metick) for the [DMALibrary](https://github.com/Metick/DMALibrary)
- [kWAYTV](https://github.com/kWAYTV) for the [dma-cheat-base](https://github.com/kWAYTV/dma-cheat-base)
- [ryanocf](https://github.com/ryanocf) for his [Mythos](https://github.com/ryanocf/Mythos)
