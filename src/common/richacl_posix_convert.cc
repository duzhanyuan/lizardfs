/*
   Copyright 2017 Skytechnology sp. z o.o.

   This file is part of LizardFS.

   LizardFS is free software: you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation, version 3.

   LizardFS is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with LizardFS. If not, see <http://www.gnu.org/licenses/>.
 */

#include "common/platform.h"
#include "common/richacl.h"

void RichACL::appendPosixACL(const AccessControlList &posix_acl, bool is_dir) {
	AccessControlList::Entry posix_ace;
	uint32_t mask;
	uint32_t x = is_dir ? 0 : Ace::kDeleteChild;

	flags_ = 0;
	owner_mask_ = 0;
	group_mask_ = 0;
	other_mask_ = 0;

	if (posix_acl.minimalAcl()) {
		// minimal Posix ACL is represented as RichACL without any Aces (masks only)
		posix_ace = posix_acl.getEntry(AccessControlList::kUser, 0);
		owner_mask_ = RichACL::convertMode2Mask(posix_ace.access_rights) & ~x;
		posix_ace = posix_acl.getEntry(AccessControlList::kGroup, 0);
		group_mask_ = RichACL::convertMode2Mask(posix_ace.access_rights) & ~x;
		posix_ace = posix_acl.getEntry(AccessControlList::kOther, 0);
		other_mask_ = RichACL::convertMode2Mask(posix_ace.access_rights) & ~x;

		flags_ |= kMasked | kWriteThrough;
		return;
	}

	// add all Aces from Posix ACL
	posix_ace = posix_acl.getEntry(AccessControlList::kUser, 0);
	mask = RichACL::convertMode2Mask(posix_ace.access_rights) & ~x;
	insert(RichACL::Ace(RichACL::Ace::kAccessAllowedAceType, RichACL::Ace::kSpecialWho, mask,
	                    RichACL::Ace::kOwnerSpecialId));

	posix_ace = posix_acl.getEntry(AccessControlList::kGroup, 0);
	mask = RichACL::convertMode2Mask(posix_ace.access_rights) & ~x;
	insert(RichACL::Ace(RichACL::Ace::kAccessAllowedAceType, RichACL::Ace::kSpecialWho, mask,
	                    RichACL::Ace::kGroupSpecialId));

	for (const auto &ace : posix_acl) {
		uint32_t mask = RichACL::convertMode2Mask(ace.access_rights) & ~x;

		if (ace.type == AccessControlList::kNamedUser) {
			insert(RichACL::Ace(RichACL::Ace::kAccessAllowedAceType, 0, mask, ace.id));
		}
		if (ace.type == AccessControlList::kNamedGroup) {
			insert(RichACL::Ace(RichACL::Ace::kAccessAllowedAceType,
			                    RichACL::Ace::kIdentifierGroup, mask, ace.id));
		}
	}

	posix_ace = posix_acl.getEntry(AccessControlList::kOther, 0);
	mask = RichACL::convertMode2Mask(posix_ace.access_rights) & ~x;
	insert(RichACL::Ace(RichACL::Ace::kAccessAllowedAceType, RichACL::Ace::kSpecialWho, mask,
	                    RichACL::Ace::kEveryoneSpecialId));

	// Isolate owner, groups and users (deny unneeded permissions from Everyone so it behaves
	// like Posix Other entry).
	isolateWho(Ace(0, Ace::kSpecialWho, 0, Ace::kOwnerSpecialId), mask);
	isolateGroupClass(mask);

	// Calculate owner, owning group and other masks (required by getMode())
	owner_mask_ = allowedToWho(Ace(0, Ace::kSpecialWho, 0, Ace::kOwnerSpecialId));
	group_mask_ = allowedToWho(Ace(0, Ace::kSpecialWho, 0, Ace::kGroupSpecialId));
	other_mask_ = allowedToWho(Ace(0, Ace::kSpecialWho, 0, Ace::kEveryoneSpecialId));

	// Posix mask ace is simulated with group mask and Group Ace in RichACL.
	// Ace Group entry stores Owning Group permissions and group_mask_
	// keeps permissions for Mask entry.
	posix_ace = posix_acl.getEntry(AccessControlList::kMask, 0);
	if (posix_ace.type != AccessControlList::kInvalid) {
		mask = RichACL::convertMode2Mask(posix_ace.access_rights) & ~x;
		group_mask_ = mask;
		flags_ |= kMasked;
	}
}

void RichACL::appendDefaultPosixACL(const AccessControlList &posix_acl) {
	RichACL dir_acl;
	uint32_t default_mask =
	    Ace::kFileInheritAce | Ace::kDirectoryInheritAce | Ace::kInheritOnlyAce;

	dir_acl.appendPosixACL(posix_acl, true);
	for (const auto &dir_ace : dir_acl) {
		RichACL::Ace ace = dir_ace;
		ace.flags |= default_mask;
		insert(ace);
	}
}

bool RichACL::hasGroupEntry() const {
	for (const auto &ace : *this) {
		if (ace.isInheritOnly()) {
			continue;
		}
		if (ace.isGroup()) {
			return true;
		}
	}

	return false;
}

std::pair<bool, AccessControlList> RichACL::convertToPosixACL() const {
	AccessControlList posix_acl;

	if (!(flags_ & kMasked) && ace_list_.empty()) {
		return std::make_pair(false, AccessControlList());
	}

	uint32_t mask, mode;
	// set owner mode flags
	mask = !(flags_ & kWriteThrough) ? allowedToWho(RichACL::Ace(0, RichACL::Ace::kSpecialWho, 0,
	                                                             RichACL::Ace::kOwnerSpecialId))
	                                 : owner_mask_;
	if (flags_ & kMasked) {
		mask &= owner_mask_;
	}
	mode = convertMask2Mode(mask) << 6;

	// set owning group mode flags
	mask =
	    allowedToWho(RichACL::Ace(0, RichACL::Ace::kSpecialWho, 0, RichACL::Ace::kGroupSpecialId));
	if ((flags_ & kWriteThrough) && !hasGroupEntry()) {
		mask = group_mask_;
	}
	mode |= (RichACL::convertMask2Mode(mask) << 3);

	// set other mode flags
	mask = !(flags_ & kWriteThrough) ? allowedToWho(RichACL::Ace(0, RichACL::Ace::kSpecialWho, 0,
	                                                             RichACL::Ace::kEveryoneSpecialId))
	                                 : other_mask_;
	if (flags_ & kMasked) {
		mask &= other_mask_;
	}
	mode |= RichACL::convertMask2Mode(mask);

	posix_acl.setMode(mode);

	for (const auto &ace : *this) {
		if (ace.isInheritOnly()) {
			continue;
		}
		if (ace.isUnixUser()) {
			// Add posix ace only on first occurrence of this user
			auto entry = posix_acl.getEntry(AccessControlList::kNamedUser, ace.id);
			if (entry.type != AccessControlList::kInvalid) {
				continue;
			}

			// approximate user permissions with one allow entry
			mask = allowedToWho(ace);
			posix_acl.setEntry(AccessControlList::kNamedUser, ace.id, convertMask2Mode(mask));
		}
		if (ace.isUnixGroup()) {
			// Add posix ace only on first occurrence of this group
			auto entry = posix_acl.getEntry(AccessControlList::kNamedGroup, ace.id);
			if (entry.type != AccessControlList::kInvalid) {
				continue;
			}

			// approximate group permissions with one allow entry
			mask = allowedToWho(ace);
			posix_acl.setEntry(AccessControlList::kNamedGroup, ace.id, convertMask2Mode(mask));
		}
	}

	if ((flags_ & kMasked) && hasGroupEntry()) {
		// Posix mask ace is simulated with group mask and Group Ace in RichACL.
		// Ace Group entry stores Owning Group permissions and group_mask_
		// keeps permissions for Mask entry.
		posix_acl.setEntry(AccessControlList::kMask, 0, convertMask2Mode(group_mask_));
	}

	return std::make_pair(true, posix_acl);
}

std::pair<bool, AccessControlList> RichACL::convertToDefaultPosixACL() const {
	RichACL rich_acl;

	for (const auto &dir_ace : *this) {
		if (!dir_ace.isInheritable()) {
			continue;
		}

		RichACL::Ace ace = dir_ace;
		ace.flags &= ~RichACL::Ace::kInheritOnlyAce;
		rich_acl.insert(ace);
	}

	std::pair<bool, AccessControlList> result;

	result = rich_acl.convertToPosixACL();
	if (result.first) {
		result.second.setEntry(AccessControlList::kMask, 0, 07);
	}
	return result;
}
