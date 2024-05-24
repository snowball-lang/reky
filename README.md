# reky
📄 Source code for Snowball's package manager!

> [!WARNING]
> This project isn't supposed to be used anywhere outside snowball's source code,
> therefor, we are using snowball's classes without having them defined here.
> e.g. snowball::utils::Logger

This code is just the backend infrastructure of the snowball package manager. Here, we:

* Update the package indexes
* Check for installed libraries
* Install required libraries
* Parse required libraries
* Scan the project for required libraries