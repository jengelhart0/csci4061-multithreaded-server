/* CSci 4061 Fall 2016 Assignment 4 */

Date: 12/09/2016
Name: Joey Engelhart

This program acts as a multi-threaded web/file server. It is run from the shell, binds to a port and accepts requests for files in a given directory via a terminal using the server's IP address.

How to Run: To run the web server, first compile the software by typing make in the directory containing the makefile.

Usage:

./web_server port_number testing_directory num_dispatchers num_workers request_queue_size cache_size.

  -port_number should be 1025 through 65535.

Requests will be logged in testing/web_server_log.

To test the web server you can try to download a file by using the command wget. To execute the following command on your machine locally run the following execution wget http://127.0.0.1:PORT/image/jpg/30.jpg. NOTE that localhost is also 127.0.0.1 and PORT is the port you binded the web server to. See testing/how_to_test for more information.

Testing for concurrency: to run multiple executions concurrently, the following command can be run. xargs -n -P. E.g., running echo $URL | xargs -n 1 -P 8 wget, will run wget 8 times simultaneously (notice the (-P 8) with 1 argument each time (-n 1)).
