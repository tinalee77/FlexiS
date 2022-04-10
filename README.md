# FlexiS

The source code used to produce the results presented in [1] is under "src".

You can find the applications used to generate greedy flows in all experiments presented in [1] under the "apps" folder.

If you want to test FlexiS with a simulator/emulator, the best way to generate high priority traffic (e.g. streaming and VoIP) is to capture real traffic and replay the trace in the simulator/emulator. FlexiS may not work well with certain built-in traffic models shipped with the simulator/emulator.

The LBE CCs including FlexiS, LEDBAT and TCP-LP have unexpected behavior when Iperf is used to generate greedy flows in the CORE emulator [2].

[1] Li, Qian (2022): TCP FlexiS: A New Approach To Incipient Congestion Detection and Control. TechRxiv.
Preprint. https://doi.org/10.36227/techrxiv.19077161.v1

[2] https://www.nrl.navy.mil/Our-Work/Areas-of-Research/Information-Technology/NCS/CORE/
