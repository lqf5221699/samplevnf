.. This work is licensed under a Creative Commons Attribution 4.0 International License.
.. http://creativecommons.org/licenses/by/4.0
.. (c) OPNFV, Intel Corporation and others.

.. OPNFV SAMPLEVNF Documentation design file.

============
Introduction
============

**Welcome to SampleVNF's developer getstarted documentation !**

.. _Pharos: https://wiki.opnfv.org/pharos
.. _SampleVNF: https://wiki.opnfv.org/samplevnf
.. _Technical_Briefs: https://wiki.opnfv.org/display/SAM/Technical+Briefs+of+VNFs

Overview:
---------

This project provides a placeholder for various sample VNF (Virtual Network Function)
development which includes example reference architecture and optimization methods
related to VNF/Network service for high performance VNFs. This project provides
benefits to other OPNFV projects like Functest, Models, yardstick etc to perform
real life use-case based testing and NFVi characterization for the same.

The sample VNFs are Open Source approximations* of Telco grade VNF’s using optimized
VNF + NFVi Infrastructure libraries, with Performance Characterization of Sample† Traffic Flows.
  * * Not a commercial product. Encourage the community to contribute and close the feature gaps.
  * † No Vendor/Proprietary Workloads 

It helps to facilitate deterministic & repeatable bench-marking on
Industry standard high volume Servers. It augments well with a Test Infrastructure
to help facilitate consistent/repeatable methodologies for characterizing & validating
the sample VNFs through OPEN SOURCE VNF approximations and test tools.
The VNFs belongs to this project are never meant for field deployment.
All the VNF source code part of this project requires Apache License Version 2.0.

Scope:
-----
The Scope of samplevnf project as follows"
To create a repository of sample VNFs to help VNF benchmarking and NFVi
characterization with real world traffic.
Host a common development environment for developing the VNF using optimized libraries
Integrate into CI tool chain and existing test frameworks for VNF feature and deployment testing

Testability:
-----------
Network Service Testing framework added into the Yardstick will be used as a test
tool for Functional/Performance verification of all the sample VNFs. 
Planning to extend the same to FuncTest and Models project to include the testcases
related to sample VNFs.
