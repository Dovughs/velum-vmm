# Security and research scope

Velum is a Windows kernel and Intel VMX research prototype for controlled systems
owned by the operator or used with authorization. Loading the driver changes
privileged execution state; experimental policies can deny operations, and
incorrect configuration or defects can crash the test machine or lose data.
Use a disposable test environment. The project is not a production security boundary.

## Questions and reports

Open a repository issue for reproducible defects, documentation corrections or
abuse concerns. Include the source revision, OS/VMM build and sanitized reproduction
steps. Do not post credentials, KDNET keys, private certificates, personal data or
unsanitized memory dumps. For sensitive reports, first ask the maintainer in an
issue to establish a private channel without disclosing exploit details; no
private reporting service or response-time commitment is currently advertised.

The project is intended for virtualization research, observability and narrowly
scoped policy experiments, not unauthorized interference with other systems.
See [GitHub's dual-use research policy](https://docs.github.com/en/site-policy/acceptable-use-policies/github-active-malware-or-exploits).
No wording or disclaimer guarantees platform acceptance; actual use and content matter.
