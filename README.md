# MiSSus. 

Fork of the [Main_MiSTer](https://github.com/MiSTer-devel/Main_MiSTer) repo.  




# Why Fork MiSTer? What did they ever do to you????


I have strong disagreements with the project management for the MiSTer project.  I realize that it is their project and as such they are allowed to run their project as terribly as they'd like but I found working with their maintainers exceptionally irritating.  

I have features I want to add and I don't want to deal with Sorg's enormous and largely undeserved ego. 

# Ok what features? 

Glad you asked.  Current features include: 

- Automatic backing up of SRAM without having to open the OSD. 
  - Can be set on an interval, default being every five minutes if there is a change in the SRAM state. 
- Journaling semantics via the use of SQLite to ensure atomicity and avoid corrupted saves in the event of an interrupted write. 
- Ability to "tag" your SRAM states and restore them later. 
- Integrity checks for the SRAM saves to ensure that your saves aren't corrupted. 
- Fallback to previous SRAM saves in the event of corruption of the latest. 


# What? SQLite? You really imported a database here? Don't you realize this is an emulation FPGA thing RAM IS LIMITED SHUT UP!

SQLite is only nominally a database; while it does provide ACID-compliant SQL, it is run in-process and works explicitly with `*.sqlite3` files. SQLite tends to be as [fast or faster](https://sqlite.org/fasterthanfs.html?utm_source=chatgpt.com) than arbitrarily reinventing and writing binary files, especially if you want ACID semantics.  

The MiSTer uses the exFAT filesystem for compatibility.  exFAT is terrible for a number of reasons, but the biggest issue is that it does not support journaling. What this ends up meaning that if saving a file is interrupted (e.g. a power outage) there is a risk of the savefile being corrupted.  OH! And you also risk the entire directory being corrupted because exFAT also might screw up the metadata because, again, it's not journaled and nothing is atomic.  

This is an issue with the *current* `Main_MiSTer` project: if the save is interrupted before completion, there is a high risk of your save being corrupted and unless you manually backed up then your progress is completely gone. 

I do not believe it's possible to make exFAT safe purely with userspace tools, but we can do things to minimize the risks and potential damage and get *pretty close* to perfect.  For example, we can treat the saves as [Append-only](https://en.wikipedia.org/wiki/Append-only) by creating new file with timestamps and a checksum upon each SRAM write, and if that's corrupted revert back to the previous safe write. 

In a hand-wavey way, this is getting into how journaling works. I could of course write my own and end up creating a journaling system or Copy-on-Write system, but at that point I think it makes sense to use a library that has these semantics out of the box, with most edge cases already worked out. SQLite does exactly that.


By utilizing SQLite's ACID guarantees and Copy-on-Write semantics, SRAM backups are "all or nothing".  Either the writing of the save was completely successful, or the write doesn't happen at all.   In that case, we revert to the last valid save, which is fine because we've been saving periodically so we would only lose N seconds of progress (where N is the interval set for automatic SRAM writing). 

*Even if you don't use the automatic snapshotting, this is still safer than the existing MiSTer setup*


# Roadmap

- On-screen keyboard to avoid having to lug out a USB keyboard to type things. 
- Notification icon on top right corner to show things are saving. 
- `.sav` file import/export. 
  - Import is already supported if a `.sav` file exists and a `.sqlite3` file does not.  The save will be imported directly into the database. 
- Oodles more things as I think of them. 







# AI Disclaimer

I make liberal use of OpenAI's Codex when working on features.  If that's a problem for you then don't use this. 
