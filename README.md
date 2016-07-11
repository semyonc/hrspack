# hrspack
This experimental audio lossless codec and archiver uses ariphmetic coding based on  
dynamic Markov modelling (Matt Mahoney entropy coder modified for signal time series compressing).

No frames, no Rice codes are used.

Novel hybrid infinite impulse response filter based on stochastic gradient with 
Metropohis-Hastings boosting is applied. 

Resulting compression rate is better/comparable to traditional lossless codecs in their maximum compression mode 
and the algorithm shows better performance.
