# cleverhome
Clever home tool(s)

Features
-View status and make simple changes from a web page
--Can augment with commandline where needed
-Full Config and Scene control either from web or commandline
-Write some type of relatively uncomplicated "Scripts" to tie together a few different devices and special logic
--Want to be able to make mods and try these out easily
--Probably python for maximum ease of making changes
-Several days worth of activity logs to help see how the system is doing
-Save what each device's config is supposed to be, and help re-sync if needed
--OZW saves what it is and tells you when it changes, but during development probably stuff will get accidentally misconfigured all the time

Design:
OZW does a lot during init.  Not suitable for small/simple commandline tools

Could make a network daemon which stays up but communicates with commandline tools
-Downside: Have to design/implement own protocol for that
-Upside: Could have the web page just run the tools

Could make a monolithic app
-Downside: Have to take it down every time changes are made (forever, not just in the beginning)
-Upside: Somewhat less code
