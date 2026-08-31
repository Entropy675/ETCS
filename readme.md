ETCS (Entity Tag Component System) is a C++ substrate for defining types, ABI contracts, capabilities, and causal relationships as one compile-time artifact. It uses ontology-compliant wrappers and MirrorBuffer boundaries to connect locally or across processes without requiring application code to duplicate the same contract in multiple systems. Functioning effectively like an operating system/virtual machine.

See https://anticurrententropy.com/whitepaper.html for details.

Currently building on linux, targeting ARM and x64

Tools & Modules
- https://github.com/Entropy675/ACE-Build-Tools 
- https://github.com/Entropy675/ETCS-Commons 

I do not recommend working on this substrate until the below todo's are complete, as they can change the active surface.

To write an ETCS provider, you effectively have to introduce yourself to the ETCS runtime via a thin ontology compliant wrapper. See ChessProvider in ETCS-Commons repo (https://github.com/Entropy675/ETCS-Commons/tree/main/ChessProvider) for a bare bones example. Once your types speak over the ETCS scripting language, you can act across the arbitrary MirrorBuffer boundary, which is the function call boundary for stream functions or streams you negotiate yourself (less recommended unless you know what you are doing - I'll make a video). The stream work function contract is an explicit lifetime: the consume stream function holds open the lifetime of the connection and is blocking; the produce side is non-blocking and expected to output continuously over the MirrorBuffer they share, which wraps/unwraps via the Wrapper_ based types owned by either side. A connection drop or otherwise failure to set up a MirrorBuffer ends the connection, logging it as a failure on both sides. If one side cannot assemble the same Wrapper_ based types required to generate a valid MirrorBuffer to a target type, stream calls fail. Since all scripts are run locally remote connections have to be brokered via a name server equivalent domain (See ChessNode from ChessProvider). MirrorBuffer covers quite literally every physical boundary from LMAX within process local buffers to sockets, the local EventNode's EventStream negotiates the provenience of local memory with the most local loader separately from the identity of the code, all entities' origin is local in every way (RIDs are runtime deterministic sequences). It's maximized for decreasing arbitrary pattern replication via the ontology, the CRTP layer reduces that pattern match to static casts. An ontology compliant wrapper of existing code gains distribution over all platforms ETCS supports, including the ETCS kernel when it releases, unless platform specific types are declared in the modules type contract declaration (the auto generated header via ace tool's module setup, Contract_XXXXXProvider.h where you can typecast unify your types for every supported system).

To build, pull ACE-Build-Tools in one folder. Pull ETCS in another folder. Pull ETCS-Commons as 'modules' folder in the ETCS folder (it's going to be a git sub-project soon, sorry for my laziness). Run:

sudo python3 ace_install.py install

That will install all the required packages, you may have to run it twice or install a missing package for some modules...
Then you can build the main etcs loader via:

```
ace make loaders
ace make loader
```
Make sure to run 'ace make loader' explicitly if you want the shell, 'ace make loaders' makes etcs without the debug flag.

You can make all the modules or a target module via:

```
ace make modules
ace make module XxxProvider
```

There are many example scripts in the /scripts/ folder, once you have compiled the modules, run etcs and enter the scripts repo, run them randomly... this side is in active development. You will likely want to use this most often for large modifications:
```
ace make modules && ace make loader

// these ones are helpful for visualizing:
ace abi
ace ontology
```


TODO:
- Import merkle hash chain from the completed version in other instance of project
- Identity system / binary signage (dependent on above)
- Test Wrapper_ & MirrorBuffer interaction thoroughly (I suspect a bug in shared memory path)
- Persistence tag within DatabaseProvider & merge Local/Remote Database ontology types
- RenderProvider and its associated ontology types (Vulkan instance, 2D target first)

Example graphical_script.etcs:
```
#!/usr/bin/env etcs

spawn WindowProvider::Window main

detach window_events.etcs window=main
main.Run(600, 600, 'GraphicalLoader Test GUI') 
main.Delete()
```

Where window_events.etcs:
```
#!/usr/bin/env etcs

requires window [Window]

window.ProduceEvents() -> window.ConsumeEvents()
window.Close()
```

You can use 'ace script print xxx.etcs' to get an expanded script printed like:
```
#!/usr/bin/env etcs

spawn WindowProvider::Window main

detach window_events.etcs window=main
{
	#!/usr/bin/env etcs
	
	requires window [Window]
	
	window.ProduceEvents() -> window.ConsumeEvents()
	window.Close()
}
main.Run(600, 600, 'GraphicalLoader Test GUI') 
main.Delete()
```

Notice how the above script uses the detached window_events.etcs script targeting the same main window to create a stream, from the event producing pump to the windows own event consuming buffer, which others can subscribe to for a event stream. The instance in WindowProvider is currently implemented via GLFW, the first detached script is the first thread of the loader so it's a valid event pump - for programs that are windowed and want input you must detach window_events.etcs first so that the input pump captures the first thread, this is an OS requirement... will not be a limitation within future ETCS kernel. Useful place to learn about the thread affinity ordering though, in case it matters to you.

The best way to think about the ETCS scripting language is as a schema for the structure of your control threads. The actual work occurs within the work functions (hence the name), ETCS scripts are called on state transitions via work functions to trigger control flow changes in the event graph. Scripts are not general programs: they declare how work functions attach to entities and how control moves on the event graph—bindings, ordering, and streaming links such as Produce -> Consume.
ETCS scripts are a non-Turing-complete description of the nameable environment under the modules currently loadable on the substrate. They are not primarily meant to be written by hand as large control-flow programs. They are meant to be assembled by naming fragments, including fragments auto-generated in bulk by causal exhaustion on the ACE build server (Target — see the whitepaper). Human authorship sits mainly at the construction layer: which pieces to attach, under which names. Where to call them, how to order the execution of scripts. Boundaries are explicit (export horizon, MirrorBuffer, one content channel). The exhaustive resolver that generates the full fragment set is work in progress.

Example: window.ProduceEvents() → window.ConsumeEvents() is not a loop in the script; it is a standing control-thread edge between two work functions.

Copyright (C) 2026 Sibte Kazmi

This library is free software; you can redistribute it and/or
modify it under the terms of the GNU Lesser General Public
License as published by the Free Software Foundation.

This library is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
Lesser General Public License for more details.

https://www.gnu.org/licenses/lgpl-3.0.html#license-text
You should have received a copy of the GNU Lesser General Public
License along with this library; if not, write to the Free Software
Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
