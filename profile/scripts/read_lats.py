import struct
import numpy as np
import sys

def main():
    fileName = 'lats.bin'
    with open(fileName, mode='rb') as file:
        fileContent = file.read()

    results = [ r for r in struct.iter_unpack("QQQ", fileContent) ]
    queue_times = [ v[0]/1e6 for v in results ]
    svc_times = [ v[1]/1e6 for v in results ]
    sjrn_times = [ v[2]/1e6 for v in results ]

    if len(sys.argv) > 1:
        for i, _ in enumerate(queue_times):
            print('{} {} {}'.format(queue_times[i], svc_times[i], sjrn_times[i]))

    print('query count: {}'.format(len(queue_times)))
    print('queue time: average {}'.format(np.mean(queue_times)))
    print('service time: average {}'.format(np.mean(svc_times)))
    print('sojourn time: average {}'.format(np.mean(sjrn_times))) 

    print('queue time: median {}, 90% {}, 95% {}, 99% {}'.format(np.median(queue_times), np.percentile(queue_times, 90), np.percentile(queue_times, 95), np.percentile(queue_times, 99)))
    print('service time: median {}, 90% {}, 95% {}, 99% {}'.format(np.median(svc_times), np.percentile(svc_times, 90), np.percentile(svc_times, 95),  np.percentile(svc_times, 99)))
    print('sojourn time: median {}, 90% {}, 95% {}, 99% {}'.format(np.median(sjrn_times), np.percentile(sjrn_times, 90), np.percentile(sjrn_times, 95),  np.percentile(sjrn_times, 99)))

if __name__ == "__main__":
     main()
