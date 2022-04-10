# FlexiS

This is the source code of TCP FlexiS. It is largely the same code that was used to produce the results presented in [1]

It compiles on Linux kernel 5.13.0-39

How to use
1. make
2. sudo make install

You can find the applications used to generate greedy flows in all experiments presented in [1] under the "apps" folder.

The LBE CCs including FlexiS, LEDBAT and TCP-LP have unexpected behavior when Iperf is used to generate greedy flows in the CORE emulator [2].

[1] Li, Qian (2022): TCP FlexiS: A New Approach To Incipient Congestion Detection and Control. TechRxiv.
Preprint. https://doi.org/10.36227/techrxiv.19077161.v1
[2] https://www.nrl.navy.mil/Our-Work/Areas-of-Research/Information-Technology/NCS/CORE/
