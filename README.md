# FlexiS

This is the source code of TCP FlexiS. 

It compiled successfully on Linux kernels v4.15.0, 5.19.0 and 6.10.0.

The code was used to produce the results presented in "TCP FlexiS: A New Approach to Incipient Congestion Detection and Control".
DOI: 10.1109/TNET.2023.3319441

If you want to use this source code to produce results to be used in your own publications, please kindly reference the GITHUB webpage or the above paper whichever you feel more appropriate.

How to compile and install

    Download the source code (tcp_flexis.c and Makefile) into a directory, e.g. flexis
    Under the directory "flexis" issue the following commands 
    make 
    sudo make install 
    The kernel module tcp_flexis should be installed and loaded after the above steps.
    Verify with lsmod | grep flexis
