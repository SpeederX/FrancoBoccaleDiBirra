This is written and maintained by human-only.

Here we define different sections, with specific meaning:

- the scope of the project - purpose / vision / scope
  - primary objective
  - features
  - extensions
- rules for modifications - rules
- the overall approach to developing new features - development workflow
  - research branch
  - feature branch
  - code practices to follow
- what to avoid during development - constraints / guardrails
  - generic behaviors or implementation approaches to avoid
  - code practices to avoid
  - smell of code or features

# the scope of the project - purpose / vision / scope

This started as a side project for fun, assembling months of research, different approaches and technologies.

## primary objective

Develop meaningful or useful feature for the user, alongside the existing ones. Meaningful means there's a semantic meaning and a scope defined enough. Vague features ideas or request are not valid and require better qualification. The purpose is not to be accomodating or let the user develop the software with unaware knowledge, they have to be kept in the process. Once you have enough information, checkpoint when reaching meaningful milestones, so you won't derail or assume things you shouldn't.

Useful questions:

- what the user asks does already exists in the app in some form? This defines if there's any need for new implementations or the scope of them.
- does it require a new feature or is it just a patch / hotfix / bug? This define the structure
  - features include extensive implementation on a vertical piece of the application, usually it's a complete new area.
  - patch involves an improvement over something already existing. it's similiar to the concept of enhancing what already exists.
  - hotfix and bug are the simpliest implementation to catch, but require a specific approach:
  - - gather useful information about version of the application, triggers, avoid using sensitive data or personal data of the user to report of fix the problem
    - define the use-case
    - replicate the problem
    - define an A / B test where A shows the bug and B shows the solution working
    - update the changelog ( if it's defined in this document a way to handle the changelog, otherwise do not add anything )
    - update the tests already existing, if none available create them. purpose is to avoid regression in the future

## features

usually in terms of what the application is now, they are new "modules" or new "modes". A specific mode rapresents a switchable cli "context" that does something different for the other existing ones.

- scratch-pad
- command
- diary
- project

these are the basic modes / features. Each of them have different purposes and approaches. They might share similiar or same technologies, but they have different end-user purpose.

scratch-pad is similiar to a whiteboard, it allows to write and interact with a model with a temporary chat. that chat can be promoted to diary which is a persistent memory mode. default for scratch-pad is that the document produced is a throw away.

diary default mode is having data-retention and data-processing on long term. it allows to store conversation, summarize them, make memories out of them, which allow to recall memories. it's meant to do and be a mirroring sort of chat which long term capabilities.

command is the dashboard with structured output to interact with the application configuration wise.

project is a coding agent, that has long term memory, uses different third party libraries to move faster and be organized.

## extensions

they are substantial improvements to the existing features, or some new modes/features. the idea is to keep features are the one provided by the application itself, extensions will be something that you can attach to the application to "extend" it with further specific usage. some example might be: job hunter, customer relationship manager.


# rules for modifications - rules

check the code thoroughly.

run the tests before each commit.

avoid commenting self-explaining code.

avoid one-liner, prefer extended code.

assume other models might work on the application, make it easy to maintain.

do not invent safety-gate, explain the user and ask how to handle ambigous situation.

Ask the user: semantic meaning of a feature, how to handle personal data or sensitive authentication tokens and their implications.

If you stumble against something that is a blocker and you've agreed with the user the implementation, surface and explain the blocker, propose your alternative and explain how it diverges from the initial scope.

# the overall approach to developing new features - development workflow

Follow standard git workflow: main/master whatever as release candidate. create a feature branch for each substantial modifications. prefer working with worktree branches separated from the main folder. the purpose is having 2 isolated environment and support multi agent features parallel development. also if you make a build, you target that specific worktree.
Always test before commit and pushing. Ensure each modification made have coverage from the tests.
Cover: false test and correct ones.
Example:
- this feature should read 2 arguments, one string, one structured object. do tests with no arguments, with 1 arguments and with 2 arguments of the wrong type; then at last build test that prove the correct workflow gets accepted.
- Manage errors and regressions first and actual implementation only after. building code, for a ui/ux feature or improvement, implies having a ui feedback.
- assume the user doesn't read your implementation and the files, it does only user-acceptance test. must be guided as someone who doesn't know the app.
- UI must enable the user, explain the commands available and have enough information to interact with the specific mode/feature.
- Make everything you can async or a job if it freezes the ui/ux.
- Bulk operation must be chunked or async by definition.
- do not sacrify correctness for speed.

## research branch

This is the only place where something "vague" and "unaware" can exist. if something is just an idea, it's by definition a research branch. it can be mature or something to brainstorm at. Mature means it has a substantial requirement but no implementation awareness. implementation awareness involves approaches like:"i wanted to create this" without specifying which technology to use or some anchor to test. Research is exactly from vague to something like: i wanted to do transcription, i know there's whisper.cpp and sherpa onnx or gemma that does any:any as input and output, and want to explore the best solution for my case. This is good because it gives anchor about technologies involved and propose already a path. Does need to be challenged, but it's a base we can work on. Features are well defined in term of technology, objective and expected performances.

## feature branch

This branch can come from a research branch that has solid foundation and gets promoted to this one, or an already well-defined requirement, design document, spec or architectural design record. there should be a candidate solution to implement the feature the user is asking: shouldn't modify other existing features, can use already existing features or technologies ( memory, transcription, whatever ).

## code practices to follow

do-not repeat yourself.

keep it simple stupid.

use configuration object over bloated if else.

Do a first implementation to try if everything works correctly and then reduce it to the minimum possible unit.

Make code modularized and reusable.

use code that is self-explanatory as preference instead of one-liner obnoxious.

Design what is needed to make it work, not for future possible extension. This can be override ONLY if the user is aware and demands a base-solution that can be later extended, be cautious.

suggest politely to be evidence driven.

define vague ideas with grounded data rather than choosing by feeling.

If there's a defined safe-gates it must be declared the reasoning behind it: percentage are allowed as exploratory initial implementation, numbers are the anchor to look for to have something to deliver.

# what to avoid during development - constraints / guardrails

Avoid WET approach.

Avoid duplication of code.

Avoid making a complete feature without single step commits - we need to be able to tell what modifications were made and why and also revert or cherry pick.

Do not take shortcut based on assumption, ask the user to solve ambigous situations.

Do not use the word "silently" or "silent" to imply something is going to happen without the user noticing. Explain in clear and thoughtful sentences the implications.

Do not praise the user for his idea.

Avoid defensive programming based on gut-feeling.

Avoid safe-gates without a data-driven approach - it might be by your chain of thoughts or the user proposal, same treatment.

Safety is crucial when you're handling sensitive data of the user: token for authentication, user personal data; make the user aware of what's going on and how their request affect the flow of data.

## generic behaviors or implementation approaches to avoid

No overengineering, build what is needed to solve what is required to reach a usable state.

Overhead it's something that requires ablation, it might happen but overall stop and think about what you're trying to achieve. Split the problem in sub-units.

Do not try to solve all the problems in one take.

## code practices to avoid

Do not speculate possible errors or exceptions and making guards about them into the code - defensive programming, base your implementation on what you're working on and use falsification and negative tests to define state that you have to guard about.

Nested multiple conditions, prefer configurations with sub objects configurable.

Avoid building RAGs without overlapped chunks.

Avoid integration of model, library and dependency in general that are made by 1 guy on github and not widely adopted ( 0 stars, no forks, not updated since 1 month more ), that means rely on something potentially unreliable.

## smell of code or features

Hardcoded or frozen one-shot paths - modification are not prototypes, they are meant to be used, maintaned and extended.

ENV vars for feature flags - they have to be enabled by the cli flags, not lost in every prompt or required to be set, let's ease up the usage.

ENV vars instead of command - prefer using command flags, as the user does not have your speed in setting up an environment. It's ok to use env vars to test something, not to deliver something the user has to use.

publicly expose tokens or sensitive authentication data.

sophisticated underlying system without a UI that exposes meaningful feedback, buttons, controls to the user.
