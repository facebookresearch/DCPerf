# AdSim: An Adaptable Benchmark for Simulating Latency-Bounded Data Serving Workloads

## Build
There are two targets under this folder and one more from treadmill
~~~
buck build @mode/opt //cea/chips/adsim/cpp2/server:adsim_server
buck build @mode/opt //cea/chips/adsim/cpp2/client:adsim_client
buck build @mode/opt //treadmill:treadmill_adsim
~~~

## Run
To launch the server, some certification is needed (so that the client can do TLS handshake upon connecting).
You can specify the path to the certifications using the command line option `--tlskey` and `--tlscert`,
or better create a symbolic link at the working directory.
~~~
ln -s /var/facebook/x509_identities/server.pem tests-cert.pem
ln -s /var/facebook/x509_identities/server.pem tests-key.pem
~~~

The server takes a configuration file, and the latest configuration tuned for Service 1 can be found under `cpp2/server/cfgs/`.
The server can be launched by the following command:
~~~
./adsim_server --config_file v19_final.json
~~~

To test the server is functioning, you can run the simple client:
~~~
./adsim_client --host <server IP>
~~~
And it should print the request response to the terminal.

To run the benchmark, please use the treadmil binary to drive it:
~~~
./treadmill_adsim --hostname <server IP> --port 10086
~~~
Note: there are a few more command line options to tune the treadmill,
and I list some of commonly used one here:
 - `--request-per-second` to specify the QPS (the latest treadmill should support fractional QPS);
 - `--number_of_workers` to specify the number of requesting threads (20 seems to be enough for AdSim);
 - `--number_of_connections` to specify the number of connections each threads creates (usually pick 2);
 - `--req_size` is specific for AdSim to control the incoming request size,
 it takes a list of number and each number in the list has the equal probability to be the size of the message sent to the AdSim server;

Here comes an example of treadmill command used to drive AdSim1 on Gen1 machine,
the benchmark for Service 1 (`--req_size` is picked according to the profile of Service 1)
~~~
./treadmill_adsim --hostname <server ip> --port 10086  --number_of_workers 20 --number_of_connections 2  --req_size 21102,24046,26718,29396,31428,33406,35250,37347,39302,40635,42162,43776,45231,46550,47941,49375,50490,51753,52789,54042,55129,56060,57210,58453,59665,60732,61951,63153,64345,65565,66788,67846,68939,70263,71538,72658,74003,75320,77021,78416,80017,81649,83231,84890,86338,87779,89549,91274,92716,94312,96249,98685,100195,102476,104590,106565,108892,112220,115426,118539,121885,125940,129406,133907,138907,144358,150643,157272,164905,173752,181697,190219,199184,208782,216714,226552,234955,242400,250273,258056,264527,274102,282639,291434,302631,314295,326790,338368,351672,365455,378563,392808,408717,428298,453281,482706,517475,566604,655912,2205630 --request-per-second 13
~~~

QPS search experiment uses the script `cpp2/client/qps_search.sh` to find the best QPS for a given target latency.
Please note the latest QPS search script should search for fractional QPS with the treadmill support to fractional QPS.
Here comes the example command:
~~~
./qps_search.sh -H <server IP> -S 21102,24046,26718,29396,31428,33406,35250,37347,39302,40635,42162,43776,45231,46550,47941,49375,50490,51753,52789,54042,55129,56060,57210,58453,59665,60732,61951,63153,64345,65565,66788,67846,68939,70263,71538,72658,74003,75320,77021,78416,80017,81649,83231,84890,86338,87779,89549,91274,92716,94312,96249,98685,100195,102476,104590,106565,108892,112220,115426,118539,121885,125940,129406,133907,138907,144358,150643,157272,164905,173752,181697,190219,199184,208782,216714,226552,234955,242400,250273,258056,264527,274102,282639,291434,302631,314295,326790,338368,351672,365455,378563,392808,408717,428298,453281,482706,517475,566604,655912,2205630 -q P95 -l 989000 -W 20 -M 8.8 -m 6.6 -R 60 -v
~~~
Other than changing `-H` to specify different host IP as you are testing different AdSim servers,
usually you need to specify the search range according to your expection to accelerate the searching process.
Use `-M` to specify the maximum QPS and `-m` to specify the minimum.
Also the target latency sometimes need to be adjusted by `-l`, which specifies the target latency in us.
And here are some of the other command line options in the example:
`-S` specifies the request size (for `--req_size` of the treadmill commands);
`-q` specifies the target latency quantile, could be `P99` or `Avg` for example;
`-W` specifies the number of workers (for `--number_of_workers` of the treadmill commands).
There are more options available, please do `qps_search.sh --help` to see the details.
