/* CSci 4061 Fall 2016 Assignment 4 */

Date: 12/09/2016
Name: Joey Engelhart

This program acts as a multi-threaded web/file server. It is run from the shell, binds to a port and accepts requests for files in a given directory via a terminal using the server's IP address.

How to Run: To run the web server, first compile the software by typing make in the directory containing the makefile.

Usage:

./web_server port_number testing_directory num_dispatchers num_workers request_queue_size cache_size.

  -port_number should be 1025 through 65535.

Requests will be logged in testing/web_server_log.

Testing information can be found in testing/how_to_test.
