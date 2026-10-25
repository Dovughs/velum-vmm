// Input-only decoded field labels shared by the kernel and console.
#pragma once
#define HV_DETAIL_LABELS { \
    { "access", "share", "create_disposition", "create_options", "file_attributes", "allocation_size_requested", "root_directory", "object_attributes", NULL, NULL, NULL, NULL }, \
    { "process_handle", "base_requested", "size_requested", "zero_bits", "allocation_type", "protection", NULL, NULL, NULL, NULL, NULL, NULL }, \
    { "access", "share", NULL, "open_options", NULL, NULL, "root_directory", "object_attributes", NULL, NULL, NULL, NULL }, \
    { "root_directory", NULL, NULL, NULL, NULL, NULL, NULL, "object_attributes", NULL, NULL, NULL, NULL }, \
    { "information_class", "input_length", "flags_or_replace", "new_size", "destination_root", NULL, NULL, NULL, NULL, NULL, NULL, NULL }, \
    { "file_handle", "bytes_requested", "byte_offset", "implicit_offset", NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL }, \
    { "access", "protection", "section_attributes", "max_size_requested", "file_handle", NULL, NULL, "object_attributes", NULL, NULL, NULL, NULL }, \
    { "section_handle", "process_handle", "base_requested", "view_size_requested", "section_offset", "zero_bits", "commit_size", "inherit", "allocation_type", "protection", NULL, NULL }, \
    { "access", "requested_pid", "requested_tid", NULL, NULL, NULL, NULL, "object_attributes", NULL, NULL, NULL, NULL }, \
    { NULL }, \
    { "process_handle", "base_requested", "size_requested", "new_protection", NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL }, \
    { NULL }, \
    { "file_handle", "bytes_requested", "byte_offset", "implicit_offset", NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL }, \
    { "pool_flags", "bytes_requested", "tag", NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL }, \
    { "pool_type", "bytes_requested", "tag", NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL }, \
    { "pointer", "tag", NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL }, \
    { "pool_type", "bytes_requested", "tag", "priority", NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL }, \
    { "pool_flags", "bytes_requested", "tag", "extended_parameters_pointer", NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL }, \
    { "pool_type", "bytes_requested", NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL }, \
    { "pointer", NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL }, \
    { "bytes_requested", "highest_physical_address", NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL }, \
    { "base_address", NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL }, \
    { "physical_address", "bytes_requested", "protection", NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL }, \
    { "base_address", "bytes_requested", NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL }, \
    { "desired_access", "source_process_handle", "source_handle", "recipient_process_handle", "handle_attributes", "duplicate_options", "source_pid", "output_handle_pointer", "desired_access_ignored", "close_source", "same_attributes", "policy_reason" }, \
    { "source_process_object", "source_address", "target_process_object", "target_address", "bytes_requested", "previous_mode_argument", "bytes_copied_output_pointer", NULL, NULL, NULL, NULL, NULL }, \
}
