# Eden Browser
#### Reimagining the web

My goal is the create the kind of browser that I want to use.  Some big features I'll be creating is a picture in picture video mode, reading lists, and adblocking.

I started implementing this in Python, using PyQt5, and was able to do a lot of awesome stuff incredibly fast.  But the ridiculousness of attempting to compile python apps into binary lead me to reimplement the browser in cpp.

### Requirements

- Qt5.8
- Seriously, that's it.


On windows you need to use the MSVC version of Qt so that WebEngine can be built.  This has been tested and built using Qt5.8, but it might work (or partially work) with previous versions.  But I don't recommend it.


### Screenshots

DevTools has been added.
![Alt text](screenshots/jan25.png?raw=true "Eden 0.1.3")

Private Windows
![Alt text](screenshots/private-window.png?raw=true "Eden 0.1.3")

I added support for some Google notifications.  A lot of improvements to be made on it.
![Alt text](screenshots/notification-shadow.png?raw=true "Eden 0.1.3")

Eden already has a partially working main menu.
![Alt text](screenshots/jan25-partial-menu.png?raw=true "Eden 0.1.3")
