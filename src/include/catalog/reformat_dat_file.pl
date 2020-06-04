#!/usr/bin/perl
#----------------------------------------------------------------------
#
# reformat_dat_file.pl
#    Perl script that reads in catalog data file(s) and writes out
#    functionally equivalent file(s) in a standard format.
#
#    In each entry of a reformatted file, metadata fields (if present)
#    come first, with normal attributes starting on the following line,
#    in the same order as the columns of the corresponding catalog.
#    Comments and blank lines are preserved.
#
# Portions Copyright (c) 1996-2019, PostgreSQL Global Development Group
# Portions Copyright (c) 1994, Regents of the University of California
#
# src/include/catalog/reformat_dat_file.pl
#
#----------------------------------------------------------------------

use strict;
use warnings;

use FindBin;
use Getopt::Long;

# If you copy this script to somewhere other than src/include/catalog,
# you'll need to modify this "use lib" or provide a suitable -I switch.
use lib "$FindBin::RealBin/../../backend/catalog/";
use Catalog;

# Names of the metadata fields of a catalog entry.
# Note: oid is a normal column from a storage perspective, but it's more
# important than the rest, so it's listed first among the metadata fields.
# Note: line_number is also a metadata field, but we never write it out,
# so it's not listed here.
my @METADATA =
  ('oid', 'oid_symbol', 'array_type_oid', 'descr', 'autogenerated');
# ADB_BEGIN
push @METADATA, 'row_macros';
# ADB_END

# Process command line switches.
my $output_path = '';
my $full_tuples = 0;

GetOptions(
	'output=s'    => \$output_path,
	'full-tuples' => \$full_tuples) || usage();

# Sanity check arguments.
die "No input files.\n" unless @ARGV;

# Make sure output_path ends in a slash.
if ($output_path ne '' && substr($output_path, -1) ne '/')
{
	$output_path .= '/';
}

# Read all the input files into internal data structures.
# We pass data file names as arguments and then look for matching
# headers to parse the schema from.
my %catalogs;
my %catalog_data;
my @catnames;
foreach my $datfile (@ARGV)
{
	$datfile =~ /(.+)\.dat$/
	  or die "Input files need to be data (.dat) files.\n";

	my $header = "$1.h";
	die "There in no header file corresponding to $datfile"
	  if !-e $header;

	my $catalog = Catalog::ParseHeader($header);
	my $catname = $catalog->{catname};
	my $schema  = $catalog->{columns};

	push @catnames, $catname;
	$catalogs{$catname} = $catalog;

	$catalog_data{$catname} = Catalog::ParseData($datfile, $schema, 1);
}

########################################################################
# At this point, we have read all the data. If you are modifying this
# script for bulk editing, this is a good place to build lookup tables,
# if you need to. In the following example, the "next if !ref $row"
# check below is a hack to filter out non-hash objects. This is because
# we build the lookup tables from data that we read using the
# "preserve_formatting" parameter.
#
##Index access method lookup.
#my %amnames;
#foreach my $row (@{ $catalog_data{pg_am} })
#{
#	next if !ref $row;
#	$amnames{$row->{oid}} = $row->{amname};
#}
########################################################################

# Write the data.
foreach my $catname (@catnames)
{
	my $catalog = $catalogs{$catname};
	my @attnames;
	my $schema = $catalog->{columns};

	foreach my $column (@$schema)
	{
		my $attname = $column->{name};

		# We may have ordinary columns at the storage level that we still
		# want to format as a special value. Exclude these from the column
		# list so they are not written twice.
		push @attnames, $attname
		  if !(grep { $_ eq $attname } @METADATA);
	}

	# Write output files to specified directory.
	my $datfile = "$output_path$catname.dat";
	open my $dat, '>', $datfile
	  or die "can't open $datfile: $!";

	foreach my $data (@{ $catalog_data{$catname} })
	{

		# Hash ref representing a data entry.
		if (ref $data eq 'HASH')
		{
			my %values = %$data;

			############################################################
			# At this point we have the full tuple in memory as a hash
			# and can do any operations we want. As written, it only
			# removes default values, but this script can be adapted to
			# do one-off bulk-editing.
			############################################################

			if (!$full_tuples)
			{
				# If it's an autogenerated entry, drop it completely.
				next if $values{autogenerated};
				# Else, just drop any default/computed fields.
				strip_default_values(\%values, $schema, $catname);
			}

			print $dat "{";

			# Separate out metadata fields for readability.
			my $metadata_str = format_hash(\%values, @METADATA);
			if ($metadata_str)
			{
				print $dat $metadata_str;

				# User attributes start on next line.
				print $dat ",\n ";
			}

			my $data_str = format_hash(\%values, @attnames);
			print $dat $data_str;
			print $dat " },\n";
		}

		# Preserve blank lines.
		elsif ($data =~ /^\s*$/)
		{
			print $dat "\n";
		}

		# Preserve comments or brackets that are on their own line.
		elsif ($data =~ /^\s*(\[|\]|#.*?)\s*$/)
		{
			print $dat "$1\n";
		}
	}
	close $dat;
}

# Remove column values for which there is a matching default,
# or if the value can be computed from other columns.
sub strip_default_values
{
	my ($row, $schema, $catname) = @_;

	# Delete values that match defaults.
	foreach my $column (@$schema)
	{
		my $attname = $column->{name};

		# It's okay if we have no oid value, since it will be assigned
		# automatically before bootstrap.
		die "strip_default_values: $catname.$attname undefined\n"
		  if !defined $row->{$attname} and $attname ne 'oid';

		if (defined $column->{default}
			and ($row->{$attname} eq $column->{default}))
		{
			delete $row->{$attname};
		}
	}

	# Delete computed values.  See AddDefaultValues() in Catalog.pm.
	# Note: This must be done after deleting values matching defaults.
	if ($catname eq 'pg_proc')
	{
		delete $row->{pronargs} if defined $row->{proargtypes};
	}

	# If a pg_type entry has an auto-generated array type, then its
	# typarray field is a computed value too (see GenerateArrayTypes).
	if ($catname eq 'pg_type')
	{
		delete $row->{typarray} if defined $row->{array_type_oid};
	}

	return;
}

# Format the individual elements of a Perl hash into a valid string
# representation. We do this ourselves, rather than use native Perl
# facilities, so we can keep control over the exact formatting of the
# data files.
sub format_hash
{
	my $data          = shift;
	my @orig_attnames = @_;

	# Copy attname to new array if it has a value, so we can determine
	# the last populated element. We do this because we may have default
	# values or empty metadata fields.
	my @attnames;
	foreach my $orig_attname (@orig_attnames)
	{
		push @attnames, $orig_attname
		  if defined $data->{$orig_attname};
	}

	# When calling this function, we ether have an open-bracket or a
	# leading space already.
	my $char_count = 1;

	my $threshold;
	my $hash_str      = '';
	my $element_count = 0;

	foreach my $attname (@attnames)
	{
		$element_count++;

		# To limit the line to 80 chars, we need to account for the
		# trailing characters.
		if ($element_count == $#attnames + 1)
		{
			# Last element, so allow space for ' },'
			$threshold = 77;
		}
		else
		{
			# Just need space for trailing comma
			$threshold = 79;
		}

		if ($element_count > 1)
		{
			$hash_str .= ',';
			$char_count++;
		}

		my $value = $data->{$attname};

		# Escape single quotes.
		$value =~ s/'/\\'/g;

		# Include a leading space in the key-value pair, since this will
		# always go after either a comma or an additional padding space on
		# the next line.
		my $element        = " $attname => '$value'";
		my $element_length = length($element);

		# If adding the element to the current line would expand the line
		# beyond 80 chars, put it on the next line. We don't do this for
		# the first element, since that would create a blank line.
		if ($element_count > 1 and $char_count + $element_length > $threshold)
		{

			# Put on next line with an additional space preceding. There
			# are now two spaces in front of the key-value pair, lining
			# it up with the line above it.
			$hash_str .= "\n $element";
			$char_count = $element_length + 1;
		}
		else
		{
			$hash_str .= $element;
			$char_count += $element_length;
		}
	}
	return $hash_str;
}

sub usage
{
	die <<EOM;
Usage: reformat_dat_file.pl [options] datafile...

Options:
    --output PATH    output directory (default '.')
    --full-tuples    write out full tuples, including default values

Non-option arguments are the names of input .dat files.
Updated files are written to the output directory,
possibly overwriting the input files.

EOM
}
