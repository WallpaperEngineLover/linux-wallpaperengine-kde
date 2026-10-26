# General scene settings
`general` of `scene.json` (`Data/Parsers/WallpaperParser.cpp`):

- orthogonalprojection: `{width, height}` (or `auto`), `null` or anything else means a perspective camera
- fov, nearz, farz: perspective camera
- clearcolor: color the scene is cleared to; ambientcolor, skylightcolor: lighting
- zoom, perspectiveoverridefov
- bloom, bloomstrength, bloomthreshold, bloomtint, hdr, bloomhdrstrength/threshold/scatter/feather/iterations
- camerafade, cameraparallax, cameraparallaxamount, cameraparallaxdelay, cameraparallaxmouseinfluence, camerapreview
- camerashake, camerashakeamplitude, camerashakeroughness, camerashakespeed
- fogdistance*/fogheight* (color, start, end and their densities): fog
- gravitydirection, gravitystrength, windenabled, winddirection, windstrength: forces rope chains and particles read
- lightconfig: number of point, spot, tube and directional lights and their shadow/cookie settings
- transparentsorting, spritesheetrefreshsync

Most of these accept scripts and animations like object properties.
