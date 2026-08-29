ETCS (Entity Tag Component System) is a C++ substrate in which type identity, ABI contract, capability boundary, and causal graph are one compile-time artifact rather than four that must be kept in agreement.
See https://anticurrententropy.com/whitepaper.html for details.

Currently building on linux, targeting ARM and x64

Tools & Modules
- https://github.com/Entropy675/ACE-Build-Tools 
- https://github.com/Entropy675/ETCS-Commons 

I do not recommend working on this substrate until the below todo's are complete, as they can change the active surface.

TODO:

- Complete strictness overhaul of ETCS language
- Fix deadlock/complete bilateral module & loader handshake
- Import merkle hash chain from the completed version in other instance of project
- Persistence tag within DatabaseProvider & merge Local/Remote Database ontology types
- RenderProvider and its associated ontology types



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
