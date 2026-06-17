# TODO

## General Mission

- [ ] Waypoint types needs to be an enum and then not a number in the UI
- [ ] Zones are awkward to create
- [ ] AI class and AI script: should be a selection, not a free input, if we can pull the options from a loadable resource (def, etc)
- [ ] "Weapon loadout" field semantics need an IDA grill (the editor panel itself is in)
- [ ] "Raw" group fields need an IDA grill and better editor integration
- [ ] Too much useless text noise in Mission tab
- [ ] Briefing needs to be a larger textbox. Also confirm whether it can be a string (rtxt) and integrate nicely if so
- [ ] "Music track" being a number is no good. Better integration
- [ ] Still no sound?

## BMS Scripting

- [ ] "PlayWavList" needs deeper editor integration
- [ ] Validate "reset after" / "pre mission" / "post mission" is implemented properly
- [ ] "ShowWaypoints" needs to be validated
- [ ] If there is no "sub-type" for an action (ie: only Null), just disable the box
- [ ] MisvarChange/Set etc need better editor integration

## Editor-wide

- [ ] The editor needs "depth": these files reference each other, and those references should be links
